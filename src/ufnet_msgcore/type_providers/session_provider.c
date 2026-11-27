/**
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <uflib/recycler/recycler.h>
#include <ufsrv_core/protocol/protocol.h>
#include <ufsrv_core/delegator_sessions/delegator_session_worker_thread.h>

#include "ufnet_msgcore/type_providers/session_provider.h"

#include <ufnetcorelib/ufnet_core/session/session_layout.h>
#include "ufnet_core/user_prefs/user_prefs.h"
#include "ufnet_msgcore/session.h"
#include <ufsrvcorelib/ufsrv_core/ufsrv_config/ufsrv_config_shim.h>
#include <ufsrv_core/delegator_sessions/session_io_state.h>


static int TypePoolGetInitCallback_Session(InstanceHolder *data_ptr, ContextData *context_data, size_t oid, unsigned long call_flags);
static int TypePoolPutInitCallback_Session(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags);
static char *TypePoolPrintCallback_Session(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags);
static int TypePoolDestructCallback_Session(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags);


static RecyclerPoolOps session_pool_ops = {
  .poolop_initget_callback = TypePoolGetInitCallback_Session,
  .poolop_initput_callback = TypePoolPutInitCallback_Session,
  .poolop_print_callback = TypePoolPrintCallback_Session,
  .poolop_destruct_callback = TypePoolDestructCallback_Session,
};

typedef struct SessionPoolProvider {
  const RecyclerPoolHandle *pool;
  type_instantiator instantiate;
  struct SessionPoolProvider *next;
} SessionPoolProvider;

/* Register pools before worker startup. The generic recycler's allocation
 * callback has no pool context, so initialise slot storage on first checkout. */
static SessionPoolProvider *session_pool_providers;

SessionStorageLayout
UfnetSessionGetLayout(void)
{
  return (SessionStorageLayout){.size = sizeof(UfnetSession), .alignment = _Alignof(UfnetSession)};
}

SessionStorageLayout
SessionGetLayout(void)
{
  return (SessionStorageLayout){.size = sizeof(Session), .alignment = _Alignof(Session)};
}

RecyclerPoolHandle *
InitSessionRecyclerTypePoolWithLayout(type_instantiator instantiator, SessionStorageLayout layout,
                                     size_t group_allocation_sz, size_t expansion_threshold)
{
  size_t alignment = RecyclerPayloadAlignment();
  if (layout.size < sizeof(Session) || layout.alignment < _Alignof(Session) ||
      alignment % layout.alignment) return NULL;

  SessionPoolProvider *provider = calloc(1, sizeof(*provider));
  if (!provider) return NULL;

  RecyclerPoolHandle *pool = RecyclerInitTypePool("Session", layout.size, group_allocation_sz,
                                                 expansion_threshold, &session_pool_ops);
  if (!pool) {
    free(provider);
    return NULL;
  }
  provider->pool = pool;
  provider->instantiate = instantiator ? instantiator : GetSessionInstantiator();
  provider->next = session_pool_providers;
  session_pool_providers = provider;
  return pool;
}

RecyclerPoolHandle *
InitSessionRecyclerTypePool(type_instantiator instantiator, size_t group_allocation_sz, size_t expansion_threshold)
{
  return InitSessionRecyclerTypePoolWithLayout(instantiator, SessionGetLayout(),
                                               group_allocation_sz, expansion_threshold);
}

static void
ResetPooledSessionProtocol(InstanceHolderForSession *holder, Session *session)
{
  if (!SESSION_PROTOCOL_SESSION_DATA(session)) return;

  const Protocol *protocol = (const Protocol *)SESSION_PROTOCOLTYPE(session);
  if (protocol && protocol->protocol_callbacks.session_reset_callback) {
    protocol->protocol_callbacks.session_reset_callback(holder, HARD_SUSPENSE);
  }
}

/**
 * @brief initialiser Each time the object is fetched from the recycler. On error, the data is automatically pushed back to the recycler.
 * and the original caller of RecyclerGet() gets NULL back.
 *
 * @param call_flags: passed down from the client through the lifecycle manager
 */
static int
TypePoolGetInitCallback_Session(InstanceHolder *data_ptr, ContextData *context_data, size_t oid, unsigned long call_flags)
{
  Session *sesn_ptr = SessionOffInstanceHolder(data_ptr);
  UfsrvSession *session = &sesn_ptr->ufsrv_session;
  const UfsrvSessionCheckoutContext *checkout = context_data;
  const Protocol *protocol = checkout ? checkout->protocol : NULL;
  if (!protocol || !protocol->session_pool) return 1;
  const RecyclerPoolHandle *pool = protocol->session_pool;

  if (session->recycler_pool != pool) {
    if (session->recycler_pool) return 1;
    const SessionPoolProvider *provider = session_pool_providers;
    while (provider && provider->pool != pool) provider = provider->next;
    if (!provider) return 1;

    SESSION_PROTOCOLTYPE(sesn_ptr) = (ProtocolTypeData *)protocol;
    ContextData *storage = sesn_ptr;
    if (provider->instantiate(&storage, CALLFLAGS_EMPTY, protocol->protocol_id) != 0) return 1;
    session->recycler_pool = pool;
    memset(&session->io_work, 0, sizeof(session->io_work));
    session->io_retry_events = 0;
    session->io_retry_after_ns = 0;
  }
  SESSION_PROTOCOLTYPE(sesn_ptr) = (ProtocolTypeData *)protocol;

  if (!(call_flags & CALL_FLAG_INSTANTIATE_FROM_SYSTEM_USER)) {
    session->session_id = UfsrvGenerateSessionIdForProtocol(protocol, &session->result, NULL);
    if (!session->session_id) return 1;
  } else {
    CloneUfsrvSystemUser(data_ptr);
  }

  //re-assign relevant values to recycled session so we can use it
  SESNSTATUS_UNSET(sesn_ptr->ufsrv_session.stat, SESNSTATUS_RECYCLED);

  if (unlikely(call_flags&CALL_FLAG_CARRIER_INSTANCE)) {
    SESNSTATUS_SET(sesn_ptr->ufsrv_session.stat, SESNSTATUS_CARRIER);
    return 0;
  }

  if (call_flags&CALL_FLAG_SNAPSHOT_INSTANCE)	SESNSTATUS_SET(sesn_ptr->ufsrv_session.stat, SESNSTATUS_SNAPSHOT);

  LoadDefaultUserPreferences(sesn_ptr);

  SESSION_SOCKETBLOCKSZ(sesn_ptr) = UfsrvConfigBufferSizesIncomingBufferSize();  //  set default read block size

  if (protocol->protocol_callbacks.session_init_callback) {
    UFSRVResult *result = protocol->protocol_callbacks.session_init_callback(sesn_ptr, 1);
    if (result && _RESULT_TYPE_ERROR(result)) return 1;
  }

  if (call_flags&CALL_FLAG_HASH_SESSION_LOCALLY) {
    if (!(AddToHash(&(SessionsDelegatorPtr()->hashed_sessions.hashtable), (void *)(InstanceHolderForSession *)data_ptr))) {
      SESNSTATUS_SET(sesn_ptr->ufsrv_session.stat, SESNSTATUS_RECYCLED);

      return 1;//object queued back automatically
    }
  }

  return 0;//success

}

/**
 * @brief: initialiser Each time the object is pushed back into the recycler.
 */
static int
TypePoolPutInitCallback_Session(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags)
{
  Session *sesn_ptr = SessionOffInstanceHolder(data_ptr);
  if (SessionIoGeneration(&sesn_ptr->ufsrv_session.io_work)) RemoveSessionToMonitoredWorkEvents(data_ptr);
  ResetPooledSessionProtocol(data_ptr, sesn_ptr);
  sesn_ptr->ufsrv_session.stat = 0;
  SESNSTATUS_SET(sesn_ptr->ufsrv_session.stat, SESNSTATUS_RECYCLED);

  if (call_flags&CALL_FLAG_HASH_SESSION_LOCALLY) {
    RemoveFromHash(&(SessionsDelegatorPtr()->hashed_sessions.hashtable), data_ptr);
  }

  return 0;//success

}

/**
 * @brief: initialiser Each time the object is pushed back into the recycler.
 */
static char *
TypePoolPrintCallback_Session(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags)
{
  Session *sesn_ptr = SessionOffInstanceHolder((InstanceHolderForSession *)data_ptr);

  return 0;//success

}

/**
 * @brief:
 */
static int
TypePoolDestructCallback_Session(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags)
{
  Session *sesn_ptr = SessionOffInstanceHolder((InstanceHolderForSession *)data_ptr);
  if (SessionIoGeneration(&sesn_ptr->ufsrv_session.io_work)) RemoveSessionToMonitoredWorkEvents(data_ptr);
  ResetPooledSessionProtocol(data_ptr, sesn_ptr);

  return 0;//success

}

void
SessionIncrementReference(InstanceHolderForSession *holder, int multiples)
{
  UfsrvSessionReference(holder, multiples);
}

void
SessionIncrementReferenceByOne(InstanceHolderForSession *holder)
{
  UfsrvSessionReference(holder, _ONCE_);
}

void
SessionDecrementReference(InstanceHolderForSession *holder, int multiples)
{
  UfsrvSessionUnreference(holder, multiples);
}

void
SessionDecrementReferenceByOne(InstanceHolderForSession *holder)
{
  UfsrvSessionUnreference(holder, _ONCE_);
}

size_t
SessionGetReferenceCount(InstanceHolderForSession *holder)
{
  return RecyclerTypeGetReferenceCount(UfsrvSessionOwnerPoolType(holder), holder);
}

int
SessionReturnToRecycler(InstanceHolderForSession *holder, ContextData *context, unsigned long flags)
{
  return UfsrvSessionReturnToRecycler(holder, context, flags);
}
