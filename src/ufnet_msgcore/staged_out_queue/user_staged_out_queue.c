/**
 * @file user_staged_out_queue.c
 * @brief Durable per-user delivery queue for client-bound commands.
 *
 * A command bound for one user (receipt, state reply, acknowledgement) is not sent from the
 * accepting thread. It is written as one record under @c USER_OUT_QUEUE:<uid>:<eid>, where @c uid is
 * the target user and @c eid is that user's event id, and a job worker later pops the record and
 * delivers it. The record grammar, the redis round trip and the discard decisions live in
 * @ref utils_staged_out_queue.h; this file supplies the user-specific facts: the key pair, the
 * user-message backend, and that the record's second prefix field is the @c fid.
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
#include <ufnet_msgcore/user/user_staged_out_queue.h>
#include <ufnet_msgcore/staged_out_queue/utils_staged_out_queue.h>
#include <uflib/utils.h>

#include "ufnet_msgcore/redis_commands_staged_messages.h"

extern __thread ThreadContext ufsrv_thread_context;

/**
 * @brief User queue identity: keyed by target user and event id, on the user-message backend.
 *
 * The record carries the fence id, because the key has no fence slot, so the second prefix field is
 * @c fid rather than the fence queue's @c gid.
 */
static const StagedOutQueueSpec sUserOutQueueSpec = {
    .queue_name          = "USER_OUT_QUEUE",
    .add_command         = REDIS_CMD_USER_OUT_QUEUE_ADD,
    .get_command         = REDIS_CMD_USER_OUT_QUEUE_GET,
    .second_field_is_fid = true
};

/**
 * @brief Stage a user-bound command for later delivery on the job pool.
 *
 * @param uid     Target user the queue belongs to; must be non-zero.
 * @param staged_message_descriptor_ptr Event identity, target snapshot and compressed payload.
 *        The event id must already have been allocated for @p uid.
 *
 * @return Through the thread context, success with the resulting queue depth as userdata, or an
 *         error code from @ref StagedOutQueueStore.
 *
 * @dynamic_memory allocates and releases the record buffers inside the shared store.
 * @locks none
 */
PUBLIC_API UFSRVResult *
StoreUserOutQueueMessage(unsigned long uid, const StagedMessageDescriptor *staged_message_descriptor_ptr)
{
  if (unlikely(IS_EMPTY(staged_message_descriptor_ptr))) THREAD_CONTEXT_RETURN_RESULT_ERROR(
    NULL, RESCODE_PROG_NULL_POINTER)

  /* <uid>:<eid> is the identifying key and must resolve; the fence id is metadata only and may be
     zero for commands that have no fence, which the shared record validator accepts */
  if (unlikely(uid == 0) || unlikely(staged_message_descriptor_ptr->eid == 0)) THREAD_CONTEXT_RETURN_RESULT_ERROR(
    NULL, RESCODE_PROG_MISSING_PARAM)

  PersistenceBackend *pers_ptr = THREAD_CONTEXT_USRMSG_CACHEBACKEND;
  if (unlikely(IS_EMPTY(pers_ptr))) THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_RESOURCE_NULL)

  return StagedOutQueueStore(&sUserOutQueueSpec, pers_ptr, uid, staged_message_descriptor_ptr->eid,
                             staged_message_descriptor_ptr);
}

/**
 * @brief Pop one staged record from a user's queue and hand back its parsed fields.
 *
 * @param uid    Target user, part of the queue key.
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
ExtractUserOutQueueMessage(unsigned long uid, unsigned long eid, StagedMessageDescriptor *staged_message_descriptor_ptr_out)
{
  if (unlikely(uid == 0) || unlikely(eid == 0)) THREAD_CONTEXT_RETURN_RESULT_ERROR(
    NULL, RESCODE_PROG_MISSING_PARAM)

  PersistenceBackend *pers_ptr = THREAD_CONTEXT_USRMSG_CACHEBACKEND;
  if (unlikely(IS_EMPTY(pers_ptr))) THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_RESOURCE_NULL)

  return StagedOutQueueExtract(&sUserOutQueueSpec, pers_ptr, uid, eid, staged_message_descriptor_ptr_out);
}
