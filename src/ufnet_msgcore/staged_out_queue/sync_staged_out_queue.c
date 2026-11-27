/**
 * @file sync_staged_out_queue.c
 * @brief Durable queue for the guardian sync copy of a message event.
 *
 * The sync copy of a message is not sent from the accepting thread. It is written as one record
 * under @c SYNC_OUT_QUEUE:<uid>:<eid>, where @c uid is the user whose event the copy belongs to and
 * @c eid is that user's freshly allocated event id, and a job worker later pops the record and
 * delivers it to that user's guardian. The record grammar, the redis round trip and the discard
 * decisions live in @ref utils_staged_out_queue.h; this file supplies the sync-specific facts: the
 * key pair, the user-message backend, and the single-recipient record shape.
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of ufnetcorelib source code.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */

#ifdef HAVE_CONFIG_UFNETCORELIB_H
#include <config_ufnetcorelib.h>
#endif

#include <main_ufnetcorelib.h>
#include <ufsrv_core/delegator_sessions/thread_context_type.h>
#include <ufsrv_core/cache_backend/redis.h>
#include <ufnet_msgcore/user/sync_staged_out_queue.h>
#include <ufnet_msgcore/staged_out_queue/utils_staged_out_queue.h>
#include <uflib/utils.h>

#include "ufnet_msgcore/redis_commands_staged_messages.h"

extern __thread ThreadContext ufsrv_thread_context;

/**
 * @brief Sync queue identity: keyed by the user whose event the copy belongs to, and its event id.
 *
 * The recipient is not part of the record: one sync copy exists per accepted event, and the
 * guardian is resolved when the record is delivered. The record therefore uses the single-recipient
 * shape and carries no fence identity.
 */
static const StagedOutQueueSpec sSyncOutQueueSpec = {
    .queue_name          = "SYNC_OUT_QUEUE",
    .add_command         = REDIS_CMD_SYNC_OUT_QUEUE_ADD,
    .get_command         = REDIS_CMD_SYNC_OUT_QUEUE_GET,
    .second_field_is_fid = false,
    .shape               = STAGED_OUT_QUEUE_SHAPE_SINGLE
};

/**
 * @brief Stage a sync copy for later delivery on the job pool.
 *
 * @param uid     User whose event the copy belongs to; must be non-zero.
 * @param staged_message_descriptor_ptr Event identity, originator and compressed payload.
 *
 * @return Through the thread context, success with the resulting queue depth as userdata, or an
 *         error code from @ref StagedOutQueueStore.
 *
 * @dynamic_memory allocates and releases the record buffers inside the shared store.
 * @locks none
 */
PUBLIC_API UFSRVResult *
StoreSyncOutQueueMessage(unsigned long uid, const StagedMessageDescriptor *staged_message_descriptor_ptr)
{
  if (unlikely(IS_EMPTY(staged_message_descriptor_ptr))) THREAD_CONTEXT_RETURN_RESULT_ERROR(
    NULL, RESCODE_PROG_NULL_POINTER)

  /* the key must resolve; the event id is the user's freshly allocated one, so it cannot be zero */
  if (unlikely(uid == 0) || unlikely(staged_message_descriptor_ptr->eid == 0)) THREAD_CONTEXT_RETURN_RESULT_ERROR(
    NULL, RESCODE_PROG_MISSING_PARAM)

  PersistenceBackend *pers_ptr = THREAD_CONTEXT_USRMSG_CACHEBACKEND;
  if (unlikely(IS_EMPTY(pers_ptr))) THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_RESOURCE_NULL)

  return StagedOutQueueStore(&sSyncOutQueueSpec, pers_ptr, uid, staged_message_descriptor_ptr->eid,
                             staged_message_descriptor_ptr);
}

/**
 * @brief Pop one staged sync record and hand back its parsed fields.
 *
 * @param uid    User whose event the queue belongs to.
 * @param eid    Event id, part of the queue key and checked against the record.
 * @param staged_message_descriptor_ptr_out Receives the parsed descriptor; on success the caller
 *        owns @c payload.uids and @c payload.payload_ptr.
 *
 * @return Through the thread context, success carrying the descriptor as userdata, or an error code
 *         from @ref StagedOutQueueExtract.
 *
 * @locks none
 */
PUBLIC_API UFSRVResult *
ExtractSyncOutQueueMessage(unsigned long uid, unsigned long eid, StagedMessageDescriptor *staged_message_descriptor_ptr_out)
{
  if (unlikely(uid == 0) || unlikely(eid == 0)) THREAD_CONTEXT_RETURN_RESULT_ERROR(
    NULL, RESCODE_PROG_MISSING_PARAM)

  PersistenceBackend *pers_ptr = THREAD_CONTEXT_USRMSG_CACHEBACKEND;
  if (unlikely(IS_EMPTY(pers_ptr))) THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_RESOURCE_NULL)

  return StagedOutQueueExtract(&sSyncOutQueueSpec, pers_ptr, uid, eid, staged_message_descriptor_ptr_out);
}
