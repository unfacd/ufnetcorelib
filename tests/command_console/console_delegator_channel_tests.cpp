#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern "C" {
#include <uflib/buffer_descriptor/buffer_descriptor.h>
#include <ufnet_msgcore/session_delegator/session_delegator_command_handler.h>
}
#include <ufnet_core/console_delegator_channel_type_priv.h>
#include <ufnet_msgcore/session_delegator/session_delegator_command.h>

#include <gtest/gtest.h>

namespace {

struct Client {
  BufferDescriptor *(*call)(ScriptHandlerContext *);
  BufferDescriptor reply;
};

UfsrvConsoleScriptHandler g_ops;
int g_long_put = 0;
int g_handler_calls = 0;

void *RunCall(void *arg)
{
  Client *client = static_cast<Client *>(arg);
  BufferDescriptorInit(&client->reply, 64);
  ScriptHandlerContext ctx = { .stemmed_path = nullptr, .user_data = nullptr,
                               .buffer = &client->reply };
  client->call(&ctx);
  return NULL;
}

extern "C" void TestHandler(const char *method, const char *path, BufferDescriptor *reply)
{
  (void)path;
  g_handler_calls++;
  if (g_long_put && method && strcmp(method, "PUT") == 0) {
    char body[601];
    memset(body, 'J', 600);
    body[600] = '\0';
    BufferDescriptorAppendFormatted(reply, "ran %s", body);
    return;
  }
  BufferDescriptorAppendFormatted(reply, "OK GOT IT -> %s", method);
}

}  // namespace

TEST(ConsoleDelegatorChannel, TrieCallbackReturnsDelegatorEcho)
{
  ConsoleDelegatorChannel channel;
  BufferDescriptor reply;
  memset(&channel, 0, sizeof channel);
  memset(&g_ops, 0, sizeof g_ops);
  ASSERT_NE(nullptr, SessionDelegatorCommandProvideHandler(&g_ops));
  ASSERT_NE(nullptr, g_ops.on_get);
  ASSERT_NE(nullptr, g_ops.on_del);
  ASSERT_NE(nullptr, g_ops.on_put);

  BufferDescriptorInit(&reply, 64);
  ScriptHandlerContext ctx = { .stemmed_path = nullptr, .user_data = nullptr,
                               .buffer = &reply };
  g_ops.on_get(&ctx);
  EXPECT_STREQ("rejected", reply.data);
  BufferDescriptorRelease(&reply);
  EXPECT_EQ(-1, ConsoleDelegatorChannelReadFd(&channel));

  ASSERT_EQ(0, ConsoleDelegatorChannelOpen(&channel, TestHandler));
  EXPECT_GE(ConsoleDelegatorChannelReadFd(&channel), 0);

  Client one;
  memset(&one, 0, sizeof one);
  one.call = g_ops.on_get;
  pthread_t thread;
  ASSERT_EQ(0, pthread_create(&thread, NULL, RunCall, &one));
  for (int spin = 0; spin < 1000000 && ConsoleDelegatorPending(&channel) < 1; spin++) sched_yield();
  ASSERT_EQ(1u, ConsoleDelegatorPending(&channel));
  char token = 0;
  ssize_t woke = -1;
  for (int spin = 0; spin < 1000000 && woke != 1; spin++) {
    woke = read(ConsoleDelegatorChannelReadFd(&channel), &token, 1);
    if (woke != 1) sched_yield();
  }
  ASSERT_EQ(1, woke);
  EXPECT_EQ('G', token);
  ConsoleDelegatorDrain(&channel);
  ASSERT_EQ(0, pthread_join(thread, NULL));
  EXPECT_STREQ("OK GOT IT -> GET", one.reply.data);
  BufferDescriptorRelease(&one.reply);

  Client del;
  memset(&del, 0, sizeof del);
  del.call = g_ops.on_del;
  ASSERT_EQ(0, pthread_create(&thread, NULL, RunCall, &del));
  for (int spin = 0; spin < 1000000 && ConsoleDelegatorPending(&channel) < 1; spin++) sched_yield();
  ASSERT_EQ(1u, ConsoleDelegatorPending(&channel));
  ConsoleDelegatorDrain(&channel);
  ASSERT_EQ(0, pthread_join(thread, NULL));
  EXPECT_STREQ("OK GOT IT -> DEL", del.reply.data);
  BufferDescriptorRelease(&del.reply);

  Client queued[CONSOLE_DELEGATOR_QUEUE_CAPACITY];
  pthread_t threads[CONSOLE_DELEGATOR_QUEUE_CAPACITY];
  memset(queued, 0, sizeof queued);
  for (size_t i = 0; i < CONSOLE_DELEGATOR_QUEUE_CAPACITY; i++) {
    queued[i].call = g_ops.on_get;
    ASSERT_EQ(0, pthread_create(&threads[i], NULL, RunCall, &queued[i]));
  }
  for (int spin = 0; spin < 1000000 && ConsoleDelegatorPending(&channel) < CONSOLE_DELEGATOR_QUEUE_CAPACITY; spin++) sched_yield();
  ASSERT_EQ(CONSOLE_DELEGATOR_QUEUE_CAPACITY, ConsoleDelegatorPending(&channel));
  BufferDescriptorInit(&reply, 64);
  ScriptHandlerContext ctx = { .stemmed_path = nullptr, .user_data = nullptr,
                               .buffer = &reply };
  g_ops.on_get(&ctx);
  EXPECT_STREQ("full", reply.data);
  BufferDescriptorRelease(&reply);
  ConsoleDelegatorDrain(&channel);
  for (size_t i = 0; i < CONSOLE_DELEGATOR_QUEUE_CAPACITY; i++) {
    ASSERT_EQ(0, pthread_join(threads[i], NULL));
    EXPECT_STREQ("OK GOT IT -> GET", queued[i].reply.data);
    BufferDescriptorRelease(&queued[i].reply);
  }
  EXPECT_EQ(0u, ConsoleDelegatorPending(&channel));

  g_long_put = 1;
  Client put;
  char long_storage[601];
  memset(&put, 0, sizeof put);
  put.call = g_ops.on_put;
  ASSERT_EQ(0, pthread_create(&thread, NULL, RunCall, &put));
  for (int spin = 0; spin < 1000000 && ConsoleDelegatorPending(&channel) < 1; spin++) sched_yield();
  ASSERT_EQ(1u, ConsoleDelegatorPending(&channel));
  ConsoleDelegatorDrain(&channel);
  ASSERT_EQ(0, pthread_join(thread, NULL));
  EXPECT_EQ(604u, put.reply.size);
  memset(long_storage, 'J', 600);
  long_storage[600] = '\0';
  EXPECT_EQ(0, strncmp(put.reply.data, "ran ", 4));
  EXPECT_STREQ(long_storage, put.reply.data + 4);
  BufferDescriptorRelease(&put.reply);
}
