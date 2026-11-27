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

#ifndef UFSRV_UTILS_STAGED_OUT_QUEUE_H
#define UFSRV_UTILS_STAGED_OUT_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <ufsrv_core/ufsrvresult_type.h>
#include <ufsrv_core/cache_backend/redis.h>
#include <ufnet_core/staged_message_descriptor_type.h>

/**
 * @file utils_staged_out_queue.h
 * @brief Shared machinery for the staged out queues.
 *
 * The fence queue (@c FENCE_OUT_QUEUE:<fid>:<eid>) and the user queue
 * (@c USER_OUT_QUEUE:<uid>:<eid>) store the same record shape and differ only in what the queue
 * belongs to, which backend it lives on, and what the second prefix field means:
 *
 * @code
 * <eid>:<second>:<userid>:<uids_sz>:<uid>:...:<raw_sz>:<attempts>:<payload>
 * @endcode
 *
 * @li @c second  the fence's @c gid on the fence queue, the command's @c fid on the user queue
 * @li @c userid  originating user id
 * @li @c payload always compressed (LZF), written and read as opaque bytes
 *
 * The sync queue (@c SYNC_OUT_QUEUE:<uid>:<eid>) has a single recipient named by its key, so it
 * stores the shorter @ref STAGED_OUT_QUEUE_SHAPE_SINGLE record:
 *
 * @code
 * <eid>:<userid>:<raw_sz>:<attempts>:<payload>
 * @endcode
 *
 * The state queue (@c STATE_OUT_QUEUE:<uid>:<eid>) fans out to a uid snapshot but has no fence, so
 * it stores the @ref STAGED_OUT_QUEUE_SHAPE_TARGETS_NO_FENCE record:
 *
 * @code
 * <eid>:<userid>:<uids_sz>:<uid>:...:<raw_sz>:<attempts>:<payload>
 * @endcode
 *
 * This module owns the prefix grammar, the redis round trip and every log/DISCARD decision, so the
 * queues can never drift apart. Callers keep their own validation policy and backend choice and
 * hand them in through @ref StagedOutQueueSpec and a resolved @c PersistenceBackend.
 */

#define STAGED_OUT_QUEUE_UID_DIGITS		(24)	///< headroom per uid in the record prefix, in decimal digits
#define STAGED_OUT_QUEUE_PREFIX_EXTRA	(160)	///< headroom for the fixed fields of the record prefix
#define STAGED_OUT_QUEUE_HEAD_LOG_SZ	(64)	///< bytes of a rejected record's ASCII head quoted in the error log

/**
 * @brief Record shape a queue stores.
 *
 * Every shape shares the event id head and the @c raw_sz / @c attempts trailer, and differs in
 * whether the record carries a fence identity and a frozen delivery snapshot:
 *
 * @code
 * TARGETS           <eid>:<second>:<userid>:<uids_sz>:<uid>:...:<raw_sz>:<attempts>:<payload>
 * TARGETS_NO_FENCE  <eid>:<userid>:<uids_sz>:<uid>:...:<raw_sz>:<attempts>:<payload>
 * SINGLE            <eid>:<userid>:<raw_sz>:<attempts>:<payload>
 * @endcode
 *
 * @c TARGETS carries both the fence identity (as its second field) and the uid snapshot.
 * @c TARGETS_NO_FENCE keeps the snapshot but belongs to an event with no fence, so its second field
 * is the originator rather than a fence id. @c SINGLE carries no snapshot at all: its queue key
 * already names the one recipient.
 */
typedef enum StagedOutQueueShape {
  STAGED_OUT_QUEUE_SHAPE_TARGETS = 0,          ///< fence identity plus the uid snapshot
  STAGED_OUT_QUEUE_SHAPE_SINGLE = 1,           ///< one recipient, named by the key; no snapshot
  STAGED_OUT_QUEUE_SHAPE_TARGETS_NO_FENCE = 2  ///< uid snapshot, no fence identity in the record
} StagedOutQueueShape;

/**
 * @brief Queue-specific facts the shared store/extract need.
 */
typedef struct StagedOutQueueSpec {
  const char *queue_name;         ///< redis key family, used verbatim in logs
  const char *add_command;        ///< RPUSH template: two unsigned longs, then `%b` payload
  const char *get_command;        ///< LPOP template: two unsigned longs
  bool        second_field_is_fid;///< true: record's second field is the fid (user queue);
                                  ///< false: it is the gid and the fence id is the first key part.
                                  ///< Used by the TARGETS shape only.
  StagedOutQueueShape shape;      ///< record shape; zero initialisation means the TARGETS shape
} StagedOutQueueSpec;

/**
 * @brief Append one formatted field to a record prefix.
 *
 * Truncation is treated as an error rather than a partial write, so a prefix that would not fit in
 * @p buffer_sz is never handed to the backend.
 *
 * @param buffer_ptr Destination prefix buffer.
 * @param buffer_sz  Total size of @p buffer_ptr.
 * @param offset     Write position, maintained by the caller across calls.
 * @param format     printf-style field format; the field must render its own ':' separator.
 * @param ...        Arguments for @p format.
 *
 * @return Bytes written, or zero when the field did not fit or formatting failed.
 */
size_t StagedOutQueuePrefixWrite(char *buffer_ptr, size_t buffer_sz, size_t offset, const char *format, ...);

/**
 * @brief Read one decimal field from a record and advance past its ':' separator.
 *
 * @param cursor_ptr Position in the record.
 * @param end_ptr    One past the end of the record.
 * @param value_out  Receives the parsed value.
 * @param next_ptr   Receives the position just after the separator.
 *
 * @return @c true only when a non-empty decimal field terminated by ':' was consumed entirely
 *         inside the record; otherwise @c false, and the record is unusable.
 */
bool StagedOutQueueParseField(const char *cursor_ptr, const char *end_ptr, size_t *value_out, const char **next_ptr);

/**
 * @brief Release a heap buffer at the end of the enclosing scope.
 *
 * Cleanup is invoked with the address of the pointer variable, so the pointee is what must be
 * released; freeing @p ptr itself would free a stack slot.
 *
 * @param ptr Address of the buffer pointer; a NULL pointer or a NULL buffer is ignored.
 */
void StagedOutQueueCleanupFree(void *ptr);

/**
 * @brief Release a redis reply at the end of the enclosing scope.
 *
 * @param reply_ptr Address of the reply pointer; a NULL pointer or a NULL reply is ignored.
 */
void StagedOutQueueCleanupReply(redisReply **reply_ptr);

/**
 * @brief Stage one record on a queue.
 *
 * Builds a record from @p staged_message_descriptor_ptr and appends it with a single command; no
 * transaction and no read-back.
 *
 * @param spec                        Queue identity: names, commands and second-field meaning.
 * @param pers_ptr                    Backend the queue lives on; chosen by the caller.
 * @param key_a                       First key part (fid or uid), substituted into the templates.
 * @param key_b                       Second key part (eid).
 * @param staged_message_descriptor_ptr Event identity, target snapshot and compressed payload.
 *        @c eid, @c userid, @c payload.payload_ptr, @c payload.payload_sz and @c payload.raw_sz are
 *        required; @c payload.uids and @c payload.uids_sz are required by the snapshot shapes only.
 *
 * @return Through the thread context, success with the resulting queue depth as userdata, or an
 *         error code from the list below.
 *
 * @retval RESCODE_PROG_NULL_POINTER        Spec, backend or descriptor was NULL.
 * @retval RESCODE_PROG_MISSING_PARAM       The originating user or a payload field was empty.
 * @retval RESCODE_PROG_MEMORY_EXHAUSTED    Record buffers could not be allocated.
 * @retval RESCODE_PROG_INCONSISTENT_DATA   A field did not fit the prefix buffer.
 * @retval RESCODE_BACKEND_CONNECTION       The backend did not answer.
 * @retval RESCODE_BACKEND_DATA             The backend answered with something other than a depth.
 *
 * @dynamic_memory allocates the prefix and the record buffer; both are released before returning.
 * @locks none
 */
UFSRVResult *StagedOutQueueStore(const StagedOutQueueSpec *spec, PersistenceBackend *pers_ptr,
                                 unsigned long key_a, unsigned long key_b,
                                 const StagedMessageDescriptor *staged_message_descriptor_ptr);

/**
 * @brief Pop one record from a queue and hand back its parsed fields.
 *
 * Removes the record before parsing, so a record that fails to parse is consumed here rather than
 * retried. Callers distinguish the outcomes by code: @c RESCODE_BACKEND_DATA_EMPTYSET means the
 * queue held nothing and nothing was consumed; parse and storage codes mean a record was consumed
 * and is now lost; connectivity codes mean nothing was consumed and the queue is untouched.
 *
 * @param spec                        Queue identity: names, commands and second-field meaning.
 * @param pers_ptr                    Backend the queue lives on; chosen by the caller.
 * @param key_a                       First key part (fid or uid).
 * @param key_b                       Second key part (eid), checked against the record.
 * @param[out] staged_message_descriptor_ptr_out Receives the parsed descriptor. On success
 *        @c payload.uids and @c payload.payload_ptr are fresh allocations owned by the caller,
 *        which must free both; the rest of the descriptor is plain values. The snapshot shapes
 *        fill the list from the record, the SINGLE shape synthesises it from @p key_a.
 *
 * @return Through the thread context, success carrying @p staged_message_descriptor_ptr_out as
 *         userdata, or an error code from the list below.
 *
 * @retval RESCODE_PROG_NULL_POINTER           Spec, backend or output descriptor was NULL.
 * @retval RESCODE_BACKEND_CONNECTION          The backend did not answer; nothing consumed.
 * @retval RESCODE_BACKEND_DATA                The backend answered with something other than a record.
 * @retval RESCODE_BACKEND_DATA_TRANSFORMATION Record was consumed but its fields did not parse,
 *         its eid did not match the key, or it carried no targets (TARGETS shape).
 * @retval RESCODE_PROG_MEMORY_EXHAUSTED       Record was consumed but could not be copied out.
 *
 * @dynamic_memory allocates the uid snapshot, the payload copy and the record buffer; the caller
 *                 owns the first two on success, everything is released on failure.
 * @locks none
 */
UFSRVResult *StagedOutQueueExtract(const StagedOutQueueSpec *spec, PersistenceBackend *pers_ptr,
                                   unsigned long key_a, unsigned long key_b,
                                   StagedMessageDescriptor *staged_message_descriptor_ptr_out);

#endif //UFSRV_UTILS_STAGED_OUT_QUEUE_H
