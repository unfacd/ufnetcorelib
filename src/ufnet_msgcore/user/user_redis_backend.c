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


#include <hiredis/hiredis.h>

#include <ufnet_msgcore/share_list.h>
#include <ufnet_msgcore/user/user_redis_backend.h>

enum { SHARE_LIST_PIPELINE_BATCH_SIZE = 256 };

bool
UserRedisShareListContainsBatch(PersistenceBackend *backend, EnumShareListType list_type,
                                const unsigned long *owner_uids, size_t count,
                                unsigned long member_id, bool *present)
{
  if (list_type < SHARELIST_PROFILE || list_type > SHARELIST_BLOCKED_FENCE || !member_id) return false;
  if (!count) return true;
  if (!backend || !backend->backend_handle_ptr || !owner_uids || !present) return false;

  redisContext *redis = backend->backend_handle_ptr;
  if (redis->err && redisReconnect(redis) != REDIS_OK) return false;

  for (size_t offset = 0; offset < count;) {
    size_t batch = count - offset;
    if (batch > SHARE_LIST_PIPELINE_BATCH_SIZE) batch = SHARE_LIST_PIPELINE_BATCH_SIZE;

    size_t queued = 0;
    while (queued < batch &&
           redisAppendCommand(redis, REDIS_CMD_SHARELIST_SCORE, (int)list_type,
                              owner_uids[offset + queued], member_id) == REDIS_OK) {
      queued++;
    }

    bool valid = queued == batch;
    for (size_t i = 0; i < queued; i++) {
      void *raw_reply = NULL;
      if (redisGetReply(redis, &raw_reply) != REDIS_OK) return false;
      redisReply *reply = raw_reply;
      if (!reply) {
        valid = false;
        continue;
      }

      if (reply->type == REDIS_REPLY_STRING) {
        present[offset + i] = true;
      } else if (reply->type == REDIS_REPLY_NIL) {
        present[offset + i] = false;
      } else {
        valid = false;
      }
      freeReplyObject(reply);
    }
    if (!valid) return false;
    offset += batch;
  }

  return true;
}
