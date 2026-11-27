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

#ifndef SRC_INCLUDE_SESSION_TYPE_H_
#define SRC_INCLUDE_SESSION_TYPE_H_

#ifdef HAVE_CONFIG_UFNETCORELIB_H
#include <config_ufnetcorelib.h>
#endif

#include "ufnet_session_type.h"
#include <ufsrv_core/session/session_state_flags.h>

#include <ufsrv_core/delegator_sessions/session_io_state.h>
#include <uflib/standard_ssl_includes.h>
#include <uflib/recycler/instance_type.h>
#ifdef MPSC_MESSAGE_QUEUE
# include <uflib/adt/adt_mpsc_queue_type.h>
#else
#include <uflib/adt/adt_queue.h>
#endif
#include <uflib/net/socket_type.h>
#include <ufsrv_core/cache_backend/persistence_type.h>
#include <uflib/db/db_sql.h>
#include <ufsrvcorelib/ufsrv_core/instrumentation/instrumentation_backend.h>
#include <ufnet_msgcore/msgqueue_backend/ufsrvmsgqueue_type.h>
#include <ufnet_msgcore/session_service_type.h>
#include <ufsrv_core/delegator_sessions/thread_context_type.h>
#include <server_geogroups_enum.h>

#include <ufsrv_core/session/sessions_utils.h>

 //special holder to encapsulate the state information that defines ephemeral mode for a given session
 typedef struct EphemeralModeDescription {
	 ThreadContext						*thread_ctx_ptr;
	 PersistenceBackend 			*pers_ptr;
	 UserMessageCacheBackend	*usrmsg_cachbackend_ptr;
	 FenceCacheBackend	 			*fence_cachbackend_ptr;
	 MessageQueueBackend 			*mq_ptr;
	 InstrumentationBackend 	*inst_ptr;
	 DbBackend								*db_backend;
 } EphemeralModeDescription;

//all x must be Session *

#define NO_SESSION                            ((Session *)NULL)

//#define SESSION_PID(x)	(x->pid)
#define AS_CARRIER_SESSION(x)                 (x) //for semantic clarity

 //these are the transitional in/out SocketMessage buffers (ie. not queueued) They don't have mutex lock associated with them

 //these are the queued SocketMessage buffers, for both incoiming and outgoing. only incoming has lock activated

#define SESSION_IN_LISTENTRY(x)	((Session *)(x->whatever))

inline static Session *
SessionOffInstanceHolder(InstanceHolderForSession *instance_holder_ptr) {
  return (Session *)GetInstance(instance_holder_ptr);
}

#endif /* SRC_INCLUDE_SESSION_TYPE_H_ */
