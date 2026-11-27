//
// Copyright (c) 2015-2026 unfacd works
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//


/**
 * @file user_redis_backend.h
 * @brief Redis operations for user records and share lists.
 */

#ifndef UFNETCORELIB_USER_REDIS_BACKEND_H
#define UFNETCORELIB_USER_REDIS_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <ufnetcorelib/ufnetcorelib_defs.h>
#include <ufnetcorelib/ufnet_msgcore/share_list_type.h>
#include <ufsrvcorelib/ufsrv_core/cache_backend/persistence_type.h>

/**
 * @brief Check whether one member belongs to each owner's share list.
 *
 * Membership checks are pipelined in bounded batches. The caller must have
 * exclusive use of the Redis connection, with no pending replies.
 *
 * @param[in,out] backend Redis persistence backend used for the pipeline.
 * @param[in] list_type Share-list type to look up for each owner.
 * @param[in] owner_uids Array of `count` owner UIDs.
 * @param[in] count Number of owners to check. An empty batch requires no connection or arrays.
 * @param[in] member_id Nonzero ID to look up in every list.
 * @param[out] present Array of `count` flags in owner order. Read only on success.
 * @return `true` if all checks completed, or `false` on invalid input or a Redis error.
 *
 * @code{.c}
 * unsigned long owners[] = {1001, 1002};
 * bool present[2];
 * if (UserRedisShareListContainsBatch(backend, SHARELIST_BLOCKED, owners, 2, 2001, present)) {
 *   bool first_owner_blocks_sender = present[0];
 * }
 * @endcode
 */
PUBLIC_API bool UserRedisShareListContainsBatch(PersistenceBackend *backend, EnumShareListType list_type,
                                                const unsigned long *owner_uids, size_t count,
                                                unsigned long member_id, bool *present);

#endif
