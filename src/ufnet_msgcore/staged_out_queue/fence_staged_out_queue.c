/**
 * @file fence_staged_out_queue.c
 * @brief Durable per-event delivery queue for messages fenced off a fence event.
 *
 * A message accepted for a fence is not sent from the accepting thread. It is written as one record
 * under @c FENCE_OUT_QUEUE:<fid>:<eid>, and a job worker later pops that record and performs the
 * fan-out. The record grammar, the redis round trip and the discard decisions live in
 * @ref utils_staged_out_queue.h; this file supplies the fence-specific facts: the key pair, the
 * fence backend and the meaning of the record's second prefix field (@c gid).
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
#include <ufnet_msgcore/fence/fence_staged_out_queue.h>
#include <ufnet_msgcore/staged_out_queue/utils_staged_out_queue.h>
#include <uflib/utils.h>

#include "ufnet_msgcore/redis_commands_staged_messages.h"

extern __thread ThreadContext ufsrv_thread_context;

/**
 * @brief Fence queue identity: keyed by fence id and event id, on the fence cache backend.
 *
 * The record's second prefix field is the event's @c gid, so extracting also recovers the fence id
 * from the key rather than from the record.
 */
static const StagedOutQueueSpec sFenceOutQueueSpec = {
    .queue_name          = "FENCE_OUT_QUEUE",
    .add_command         = REDIS_CMD_FENCE_OUT_QUEUE_ADD,
    .get_command         = REDIS_CMD_FENCE_OUT_QUEUE_GET,
    .second_field_is_fid = false
};

/**
 * @brief Stage a fence message for later delivery on the job pool.
 *
 * @param staged_message_descriptor_ptr Event identity, target snapshot and compressed payload.
 *
 * @return Through the thread context, success with the resulting queue depth as userdata, or an
 *         error code from @ref StagedOutQueueStore.
 *
 * @dynamic_memory allocates and releases the record buffers inside the shared store.
 * @locks none
 */
PUBLIC_API UFSRVResult *
StoreFenceOutQueueMessage(const StagedMessageDescriptor *staged_message_descriptor_ptr)
{
  if (unlikely(IS_EMPTY(staged_message_descriptor_ptr))) THREAD_CONTEXT_RETURN_RESULT_ERROR(
    NULL, RESCODE_PROG_NULL_POINTER)

  PersistenceBackend *pers_ptr = THREAD_CONTEXT_FENCE_CACHEBACKEND;
  if (unlikely(IS_EMPTY(pers_ptr))) THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_RESOURCE_NULL)

  return StagedOutQueueStore(&sFenceOutQueueSpec, pers_ptr, staged_message_descriptor_ptr->fid,
                             staged_message_descriptor_ptr->eid, staged_message_descriptor_ptr);
}

/**
 * @brief Pop one staged fence record for an event and hand back its parsed fields.
 *
 * @param fid    Fence id, part of the queue key.
 * @param eid    Fence event id, part of the queue key and checked against the record.
 * @param staged_message_descriptor_ptr_out Receives the parsed descriptor; on success the caller
 *        owns @c payload.uids and @c payload.payload_ptr.
 *
 * @return Through the thread context, success carrying the descriptor as userdata, or an error code
 *         from @ref StagedOutQueueExtract.
 *
 * @locks none
 */
PUBLIC_API UFSRVResult *
ExtractFenceOutQueueMessage(unsigned long fid, unsigned long eid, StagedMessageDescriptor *staged_message_descriptor_ptr_out)
{
  PersistenceBackend *pers_ptr = THREAD_CONTEXT_FENCE_CACHEBACKEND;
  if (unlikely(IS_EMPTY(pers_ptr))) THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_RESOURCE_NULL)

  return StagedOutQueueExtract(&sFenceOutQueueSpec, pers_ptr, fid, eid, staged_message_descriptor_ptr_out);
}
