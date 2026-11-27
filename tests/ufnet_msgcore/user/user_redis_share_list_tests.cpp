#include <cstdarg>
#include <limits>
#include <vector>
#include <gtest/gtest.h>

extern "C" {
#include <hiredis/hiredis.h>
#include <ufnet_msgcore/user/user_redis_backend.h>
}

namespace {

struct ScoreRequest {
  int type;
  unsigned long owner;
  unsigned long member;
};

constexpr size_t NoFailure = std::numeric_limits<size_t>::max();

struct RedisMock {
  std::vector<ScoreRequest> requests;
  std::vector<int> reply_types;
  std::vector<size_t> batch_sizes;
  size_t append_failure = NoFailure;
  size_t read_failure = NoFailure;
  size_t reads = 0;
  size_t freed = 0;
  size_t pending = 0;
  bool reading_batch = false;
  int reconnect_result = REDIS_OK;
  int reconnects = 0;
  redisReply reply{};
};

RedisMock mock;

class ShareListPipeline : public testing::Test {
protected:
  void SetUp() override {
    mock = {};
    backend.backend_handle_ptr = &redis;
  }

  redisContext redis{};
  PersistenceBackend backend{};
};

}

extern "C" int redisAppendCommand(redisContext *, const char *format, ...)
{
  EXPECT_STREQ(format, "ZSCORE SHL_%i:%lu %lu");
  EXPECT_FALSE(mock.reading_batch);
  if (mock.requests.size() == mock.append_failure) return REDIS_ERR;

  va_list args;
  va_start(args, format);
  ScoreRequest request{va_arg(args, int), va_arg(args, unsigned long), va_arg(args, unsigned long)};
  va_end(args);
  mock.requests.push_back(request);
  mock.pending++;
  return REDIS_OK;
}

extern "C" int redisGetReply(redisContext *redis, void **reply)
{
  EXPECT_GT(mock.pending, 0u);
  if (!mock.reading_batch) {
    mock.batch_sizes.push_back(mock.pending);
    mock.reading_batch = true;
  }
  if (mock.reads == mock.read_failure) {
    redis->err = REDIS_ERR_IO;
    return REDIS_ERR;
  }
  mock.reply.type = mock.reply_types.at(mock.reads++);
  *reply = &mock.reply;
  mock.reading_batch = --mock.pending != 0;
  return REDIS_OK;
}

extern "C" void freeReplyObject(void *reply)
{
  EXPECT_EQ(reply, &mock.reply);
  mock.freed++;
}

extern "C" int redisReconnect(redisContext *redis)
{
  mock.reconnects++;
  if (mock.reconnect_result == REDIS_OK) {
    redis->err = 0;
    mock.pending = 0;
    mock.reading_batch = false;
  }
  return mock.reconnect_result;
}

TEST_F(ShareListPipeline, PreservesOwnerOrderAcrossBoundedBatches)
{
  unsigned long owners[520];
  bool present[520]{};
  for (size_t i = 0; i < 520; i++) {
    owners[i] = 100 + i % 257;
    mock.reply_types.push_back(owners[i] % 2 ? REDIS_REPLY_NIL : REDIS_REPLY_STRING);
  }

  ASSERT_TRUE(UserRedisShareListContainsBatch(&backend, SHARELIST_LOCATION, owners, 520, 42, present));
  EXPECT_EQ(mock.batch_sizes, (std::vector<size_t>{256, 256, 8}));
  ASSERT_EQ(mock.requests.size(), 520u);
  for (size_t i = 0; i < 520; i++) {
    EXPECT_EQ(mock.requests[i].type, SHARELIST_LOCATION);
    EXPECT_EQ(mock.requests[i].owner, owners[i]);
    EXPECT_EQ(mock.requests[i].member, 42u);
    EXPECT_EQ(present[i], owners[i] % 2 == 0);
  }
  EXPECT_EQ(mock.freed, 520u);
}

TEST_F(ShareListPipeline, DrainsQueuedRepliesAfterRedisError)
{
  const unsigned long owners[] = {100, 101, 102};
  bool present[3]{};
  mock.reply_types = {REDIS_REPLY_NIL, REDIS_REPLY_ERROR, REDIS_REPLY_STRING};

  EXPECT_FALSE(UserRedisShareListContainsBatch(&backend, SHARELIST_BLOCKED, owners, 3, 42, present));
  EXPECT_EQ(mock.pending, 0u);
  EXPECT_EQ(mock.freed, 3u);

  mock.reply_types.push_back(REDIS_REPLY_STRING);
  EXPECT_TRUE(UserRedisShareListContainsBatch(&backend, SHARELIST_BLOCKED, owners, 1, 42, present));
  EXPECT_TRUE(present[0]);
}

TEST_F(ShareListPipeline, DrainsCommandsAppendedBeforeAppendFailure)
{
  const unsigned long owners[] = {100, 101, 102};
  bool present[3]{};
  mock.append_failure = 2;
  mock.reply_types = {REDIS_REPLY_NIL, REDIS_REPLY_STRING};

  EXPECT_FALSE(UserRedisShareListContainsBatch(&backend, SHARELIST_BLOCKED, owners, 3, 42, present));
  EXPECT_EQ(mock.requests.size(), 2u);
  EXPECT_EQ(mock.pending, 0u);
  EXPECT_EQ(mock.freed, 2u);
}

TEST_F(ShareListPipeline, StopsOnTransportFailureAndReconnectsOnNextCall)
{
  const unsigned long owners[] = {100, 101, 102};
  bool present[3]{};
  mock.reply_types = {REDIS_REPLY_NIL, REDIS_REPLY_STRING};
  mock.read_failure = 1;
  EXPECT_FALSE(UserRedisShareListContainsBatch(&backend, SHARELIST_BLOCKED, owners, 3, 42, present));
  EXPECT_EQ(mock.freed, 1u);
  EXPECT_EQ(mock.reconnects, 0);

  mock.reconnect_result = REDIS_ERR;
  EXPECT_FALSE(UserRedisShareListContainsBatch(&backend, SHARELIST_BLOCKED, owners, 1, 42, present));
  EXPECT_EQ(mock.requests.size(), 3u);
  mock.reconnect_result = REDIS_OK;
  mock.read_failure = NoFailure;
  EXPECT_TRUE(UserRedisShareListContainsBatch(&backend, SHARELIST_BLOCKED, owners, 1, 42, present));
  EXPECT_TRUE(present[0]);
  EXPECT_EQ(mock.reconnects, 2);
}

TEST_F(ShareListPipeline, ValidatesArgumentsBeforeCallingRedis)
{
  const unsigned long owner = 100;
  bool present = false;
  EXPECT_TRUE(UserRedisShareListContainsBatch(nullptr, SHARELIST_PROFILE, nullptr, 0, 42, nullptr));
  EXPECT_FALSE(UserRedisShareListContainsBatch(nullptr, SHARELIST_PROFILE, &owner, 1, 42, &present));
  EXPECT_FALSE(UserRedisShareListContainsBatch(&backend, SHARELIST_PROFILE, nullptr, 1, 42, &present));
  EXPECT_FALSE(UserRedisShareListContainsBatch(&backend, SHARELIST_PROFILE, &owner, 1, 42, nullptr));
  EXPECT_FALSE(UserRedisShareListContainsBatch(&backend, SHARELIST_PROFILE, &owner, 1, 0, &present));
  EXPECT_FALSE(UserRedisShareListContainsBatch(&backend, static_cast<EnumShareListType>(9), &owner, 1, 42, &present));
  EXPECT_TRUE(mock.requests.empty());
}
