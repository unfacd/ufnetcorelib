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

#ifndef UFSRV_SYNC_STAGED_OUT_QUEUE_H
#define UFSRV_SYNC_STAGED_OUT_QUEUE_H

#include <stddef.h>
#include <ufnetcorelib_defs.h>
#include <ufsrv_core/ufsrvresult_type.h>
#include <ufnet_core/session/session_type.h>
#include <ufnet_core/staged_message_descriptor_type.h>

/**
 * @file sync_staged_out_queue.h
 * @brief Durable queue for the guardian sync copy of a message event.
 *
 * The sync copy is not sent from the accepting thread. It is written here as one record under
 * @c SYNC_OUT_QUEUE:<uid>:<eid>, where @c uid is the user whose event the copy belongs to and
 * @c eid is that user's freshly allocated event id, and a job worker later pops the record and
 * delivers it to the guardian attached to that user.
 *
 * @section recordformat Record format
 *
 * @code
 * <eid>:<userid>:<raw_sz>:<attempts>:<payload>
 * @endcode
 *
 * @li @c eid          event id, must match the key; allocated for the user the sync belongs to
 * @li @c userid       originating user id, metadata about who caused the event and the user whose
 *                     guardian receives the copy
 * @li @c raw_sz       size of @c payload once decompressed
 * @li @c attempts     delivery attempts already spent on this record, zero when first staged
 * @li @c payload      compressed command bytes; everything in this queue is assumed compressed
 *
 * The record carries no target list: the key names the user, and the recipient is that user's
 * guardian, resolved when the record is delivered. Delivery is therefore bound to the guardian
 * relation as it stands at drain time, not as it stood when the event was accepted.
 */

/**
 * @brief Stage a sync copy for later delivery on the job pool.
 *
 * @param uid     User whose event the copy belongs to; must be non-zero.
 * @param staged_message_descriptor_ptr Event identity, originator and compressed payload.
 *        @c eid, @c userid, @c payload.payload_ptr, @c payload.payload_sz and @c payload.raw_sz are
 *        required. No target snapshot is stored.
 *
 * @return Through the thread context, success with the resulting queue depth as userdata, or one of
 *         the error codes below.
 *
 * @retval RESCODE_PROG_NULL_POINTER        Descriptor was NULL.
 * @retval RESCODE_PROG_MISSING_PARAM       The queue uid, event id or a payload field was empty.
 * @retval RESCODE_BACKEND_RESOURCE_NULL    No user-message cache backend on this thread.
 * @retval RESCODE_PROG_MEMORY_EXHAUSTED    Record buffers could not be allocated.
 * @retval RESCODE_PROG_INCONSISTENT_DATA   A field did not fit the prefix buffer.
 * @retval RESCODE_BACKEND_CONNECTION       The backend did not answer.
 * @retval RESCODE_BACKEND_DATA             The backend answered with something other than a depth.
 *
 * @dynamic_memory allocates the prefix and the record buffer; both are released before returning.
 * @locks none
 */
PUBLIC_API UFSRVResult *StoreSyncOutQueueMessage(unsigned long uid,
                                                 const StagedMessageDescriptor *staged_message_descriptor_ptr);

/**
 * @brief Pop one staged record from a sync queue and hand back its parsed fields.
 *
 * Removes the record from @c SYNC_OUT_QUEUE:<uid>:<eid> before parsing, so a record that fails to
 * parse is consumed here rather than retried. Callers distinguish the outcomes by code:
 * @c RESCODE_BACKEND_DATA_EMPTYSET means the queue held nothing and nothing was consumed; parse and
 * storage codes mean a record was consumed and is now lost; connectivity codes mean nothing was
 * consumed and the queue is untouched.
 *
 * @param uid    User whose event the queue belongs to.
 * @param eid    Event id, part of the queue key and checked against the record.
 * @param[out] staged_message_descriptor_ptr_out Receives the parsed descriptor. On success
 *             @c payload.uids holds the synthesised single recipient (@p uid) and
 *             @c payload.payload_ptr the compressed bytes; both are fresh allocations owned by the
 *             caller, which must free both. The rest of the descriptor is plain values.
 *
 * @return Through the thread context, success carrying @p staged_message_descriptor_ptr_out as
 *         userdata, or one of the error codes below.
 *
 * @retval RESCODE_BACKEND_DATA_EMPTYSET       Queue was empty; nothing consumed.
 * @retval RESCODE_PROG_NULL_POINTER           Output descriptor was NULL.
 * @retval RESCODE_BACKEND_RESOURCE_NULL       No user-message cache backend on this thread.
 * @retval RESCODE_BACKEND_CONNECTION          The backend did not answer; nothing consumed.
 * @retval RESCODE_BACKEND_DATA                The backend answered with something other than a record.
 * @retval RESCODE_BACKEND_DATA_TRANSFORMATION Record was consumed but its fields did not parse or
 *         its eid did not match the key.
 * @retval RESCODE_PROG_MEMORY_EXHAUSTED       Record was consumed but could not be copied out.
 *
 * @dynamic_memory allocates the recipient, the payload copy and the record buffer; the caller owns
 *                 the first two on success, everything is released on failure.
 * @locks none
 */
PUBLIC_API UFSRVResult *ExtractSyncOutQueueMessage(unsigned long uid, unsigned long eid,
                                                   StagedMessageDescriptor *staged_message_descriptor_ptr_out);

#endif //UFSRV_SYNC_STAGED_OUT_QUEUE_H
