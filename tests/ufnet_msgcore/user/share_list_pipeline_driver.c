#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <hiredis/hiredis.h>
#include <ufnet_msgcore/user/user_redis_backend.h>

static void
ExpectStatus(redisContext *redis, const char *command, const char *expected)
{
  redisReply *reply = redisCommand(redis, command);
  assert(reply && reply->type == REDIS_REPLY_STATUS && strcmp(reply->str, expected) == 0);
  freeReplyObject(reply);
}

int
main(int argc, char **argv)
{
  assert(argc == 2);
  redisContext *redis = redisConnectWithTimeout("127.0.0.1", atoi(argv[1]), (struct timeval){.tv_sec = 5});
  assert(redis && !redis->err);
  assert(redisSetTimeout(redis, (struct timeval){.tv_sec = 5}) == REDIS_OK);
  PersistenceBackend backend = {.backend_handle_ptr = redis};

  assert(UserRedisShareListContainsBatch(NULL, SHARELIST_PROFILE, NULL, 0, 42, NULL));
  assert(!UserRedisShareListContainsBatch(NULL, SHARELIST_PROFILE, NULL, 1, 42, NULL));
  assert(!UserRedisShareListContainsBatch(&backend, (EnumShareListType)99, NULL, 0, 42, NULL));
  assert(!UserRedisShareListContainsBatch(&backend, SHARELIST_PROFILE, NULL, 0, 0, NULL));

  enum { OWNER_COUNT = 520 };
  unsigned long owners[OWNER_COUNT];
  bool present[OWNER_COUNT];
  for (size_t i = 0; i < OWNER_COUNT; i++) {
    owners[i] = 1000 + i;
    if (i % 3 == 0) {
      redisReply *reply = redisCommand(redis, "ZADD SHL_0:%lu 0 %lu", owners[i], 42UL);
      assert(reply && reply->type == REDIS_REPLY_INTEGER && reply->integer == 1);
      freeReplyObject(reply);
    }
  }

  assert(UserRedisShareListContainsBatch(&backend, SHARELIST_PROFILE, owners, OWNER_COUNT, 42, present));
  for (size_t i = 0; i < OWNER_COUNT; i++) assert(present[i] == (i % 3 == 0));

  const unsigned long repeated[] = {owners[3], owners[2], owners[3], owners[0]};
  assert(UserRedisShareListContainsBatch(&backend, SHARELIST_PROFILE, repeated, 4, 42, present));
  assert(present[0] && !present[1] && present[2] && present[3]);
  assert(UserRedisShareListContainsBatch(&backend, SHARELIST_BLOCKED, repeated, 4, 42, present));
  for (size_t i = 0; i < 4; i++) assert(!present[i]);

  ExpectStatus(redis, "SET SHL_5:1002 wrong-type", "OK");
  assert(!UserRedisShareListContainsBatch(&backend, SHARELIST_BLOCKED, repeated, 4, 42, present));
  ExpectStatus(redis, "PING", "PONG");
  assert(UserRedisShareListContainsBatch(&backend, SHARELIST_PROFILE, repeated, 4, 42, present));
  assert(present[0] && !present[1] && present[2] && present[3]);

  redisReply *id = redisCommand(redis, "CLIENT ID");
  assert(id && id->type == REDIS_REPLY_INTEGER);
  redisContext *admin = redisConnect("127.0.0.1", atoi(argv[1]));
  assert(admin && !admin->err);
  redisReply *killed = redisCommand(admin, "CLIENT KILL ID %lld", id->integer);
  assert(killed && killed->type == REDIS_REPLY_INTEGER && killed->integer == 1);
  freeReplyObject(killed);
  freeReplyObject(id);
  redisFree(admin);

  assert(!UserRedisShareListContainsBatch(&backend, SHARELIST_PROFILE, repeated, 4, 42, present));
  assert(UserRedisShareListContainsBatch(&backend, SHARELIST_PROFILE, repeated, 4, 42, present));
  assert(present[0] && !present[1] && present[2] && present[3]);
  redisFree(redis);
  return 0;
}
