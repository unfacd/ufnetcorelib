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

#ifndef UFSRV_USER_STAGED_OUT_QUEUE_H
#define UFSRV_USER_STAGED_OUT_QUEUE_H

#include <stddef.h>
#include <ufnetcorelib_defs.h>
#include <ufsrv_core/ufsrvresult_type.h>
#include <ufnet_core/session/session_type.h>
#include <ufnet_core/staged_message_descriptor_type.h>

/**
 * @file user_staged_out_queue.h
 * @brief Durable per-user delivery queue for client-bound commands.
 *
 * A user-bound command (receipt, state reply, acknowledgement) is not sent from the accepting
 * thread. It is written here as one record under @c USER_OUT_QUEUE:<uid>:<eid>, where @c uid is the
 * target user the queue belongs to and @c eid makes the key globally unique, and a job worker later
 * pops it and delivers it.
 *
 * @section recordformat Record format
 *
 * @code
 * <eid>:<fid>:<userid>:<uids_sz>:<uid>:...:<raw_sz>:<attempts>:<payload>
 * @endcode
 *
 * @li @c eid          event id, must match the key; the target user's event counter provides it
 * @li @c fid          fence the command belongs to, carried here because the key has no fence slot;
 *                     zero when the command has no fence, since it is metadata only
 * @li @c userid       originating user id, metadata about who caused the command
 * @li @c uids_sz      number of target uids that follow
 * @li @c uid          one per delivery target, snapshotted when the record was staged (usually one)
 * @li @c raw_sz       size of @c payload once decompressed
 * @li @c attempts     delivery attempts already spent on this record, zero when first staged
 * @li @c payload      compressed command bytes; everything in this queue is assumed compressed
 *
 * The prefix mirrors the fence out queue and is positional: writer and reader change together.
 * The target user appears in the key, not in the prefix, so a queue belongs to exactly one user.
 */

/**
 * @brief Stage a user-bound command for later delivery on the job pool.
 *
 * @param uid     Target user the queue belongs to; must be non-zero.
 * @param staged_message_descriptor_ptr Event identity, target snapshot and compressed payload.
 *        @c eid, @c fid, @c userid, @c payload.uids, @c payload.uids_sz, @c payload.payload_ptr,
 *        @c payload.payload_sz and @c payload.raw_sz are required.
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
PUBLIC_API UFSRVResult *StoreUserOutQueueMessage(unsigned long uid,
                                                 const StagedMessageDescriptor *staged_message_descriptor_ptr);

/**
 * @brief Pop one staged record from a user's queue and hand back its parsed fields.
 *
 * Removes the record from @c USER_OUT_QUEUE:<uid>:<eid> before parsing, so a record that fails to
 * parse is consumed here rather than retried. Callers distinguish the outcomes by code:
 * @c RESCODE_BACKEND_DATA_EMPTYSET means the queue held nothing and nothing was consumed; parse and
 * storage codes mean a record was consumed and is now lost; connectivity codes mean nothing was
 * consumed and the queue is untouched.
 *
 * @param uid    Target user, part of the queue key.
 * @param eid    Event id, part of the queue key and checked against the record.
 * @param[out] staged_message_descriptor_ptr_out Receives the parsed descriptor. On success
 *             @c payload.uids and @c payload.payload_ptr are fresh allocations owned by the caller,
 *             which must free both; the rest of the descriptor is plain values.
 *
 * @return Through the thread context, success carrying @p staged_message_descriptor_ptr_out as
 *         userdata, or one of the error codes below.
 *
 * @retval RESCODE_BACKEND_DATA_EMPTYSET       Queue was empty; nothing consumed.
 * @retval RESCODE_PROG_NULL_POINTER           Output descriptor was NULL.
 * @retval RESCODE_BACKEND_RESOURCE_NULL       No user-message cache backend on this thread.
 * @retval RESCODE_BACKEND_CONNECTION          The backend did not answer; nothing consumed.
 * @retval RESCODE_BACKEND_DATA                The backend answered with something other than a record.
 * @retval RESCODE_BACKEND_DATA_TRANSFORMATION Record was consumed but its fields did not parse,
 *         its eid did not match the key, or it carried no targets.
 * @retval RESCODE_PROG_MEMORY_EXHAUSTED       Record was consumed but could not be copied out.
 *
 * @dynamic_memory allocates the uid snapshot, the payload copy and the record buffer; the caller
 *                 owns the first two on success, everything is released on failure.
 * @locks none
 */
PUBLIC_API UFSRVResult *ExtractUserOutQueueMessage(unsigned long uid, unsigned long eid,
                                                   StagedMessageDescriptor *staged_message_descriptor_ptr_out);

#endif //UFSRV_USER_STAGED_OUT_QUEUE_H
