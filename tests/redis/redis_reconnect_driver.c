/**
 * @file redis_reconnect_driver.c
 * @brief Standalone probe for the Redis connection-loss retry loop (Toxiproxy L3).
 *
 * Links the ufnetcorelib redis cache-backend (redis.c + persistence.c) and the
 * vendored hiredis + uflib, opens a connection through a Toxiproxy front, and
 * drives RedisSendCommand the way a real consumer does — so the fault injected
 * by toxiproxy_redis_reconnect.sh exercises the *production* reconnect/backoff
 * path byte for byte.
 *
 * Design:
 *   - The driver supplies the thread-local `ufsrv_thread_context` the library
 *     only extern-declares (its definition normally lives in the app server).
 *   - Backoff is observed by wrapping nanosleep (`-Wl,--wrap=nanosleep`):
 *     sReconnectRedisBackendWithBackoff sleeps through nanosleep, so
 *     __wrap_nanosleep records each deterministic delay (1000/2000/4000/8000/10000).
 *   - A READY line (flushed) is printed before the blocking command so the
 *     script knows the exact window in which to inject the fault.
 *
 * Scenarios (--expect):
 *   connect_down  — the endpoint is down, so the connect returns NULL.
 *   exhaustion    — a persistent reset makes every reconnect fail; after the
 *                   full backoff sequence the command returns NULL.
 *   recovery      — a transient outage is healed within one backoff; the
 *                   command returns a reply.
 *
 * Usage:
 *   redis_reconnect_driver --host 127.0.0.1 --port 63790 \
 *       --label uflib-test --expect exhaustion
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <ufnet_core/cache_backend/redis.h>
#include <ufnet_core/cache_backend/persistence.h>
#include <ufnet_core/thread_context_type.h>

/* The thread-local the library extern-declares; the app server normally owns it. */
__thread ThreadContext ufsrv_thread_context;

#define MAX_BACKOFFS  32

/* Recorded backoff delays (milliseconds) and how many were observed. */
static long    s_backoff_delays[MAX_BACKOFFS];
static size_t  s_backoff_count = 0;

static long long
now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000);
}

/*
 * Linker --wrap=nanosleep target.  sReconnectRedisBackendWithBackoff sleeps via
 * nanosleep, so every backoff delay passes through here; record and print it,
 * then perform the real sleep.
 */
int __real_nanosleep(const struct timespec *req, struct timespec *rem);

int
__wrap_nanosleep(const struct timespec *req, struct timespec *rem)
{
  long delay_ms = (long)req->tv_sec * 1000L + (long)req->tv_nsec / 1000000L;

  /* Backoff delays are >= 1000 ms; ignore incidental sub-millisecond sleeps. */
  if (delay_ms >= 100) {
    if (s_backoff_count < MAX_BACKOFFS) {
      s_backoff_delays[s_backoff_count++] = delay_ms;
    }
    printf("[%lld] BACKOFF %ld\n", now_ms(), delay_ms);
    fflush(stdout);
  }

  return __real_nanosleep(req, rem);
}

static void
usage(const char *prog)
{
  fprintf(stderr,
          "usage: %s --host H --port P [--label L] "
          "[--expect connect_down|exhaustion|recovery|recovery_depth|store_result_loss]\n",
          prog);
}

/* Assert that the recorded backoff delays exactly match the expected sequence. */
static int
sAssertBackoffSequence(const char *scenario, const long *expected, size_t n)
{
  int verdict = 0;

  if (s_backoff_count != n) {
    fprintf(stderr, "%s: expected %zu backoffs, got %zu\n", scenario, n, s_backoff_count);
    verdict = 1;
  }
  for (size_t i = 0; i < n && i < s_backoff_count; ++i) {
    if (s_backoff_delays[i] != expected[i]) {
      fprintf(stderr, "%s: backoff[%zu] expected %ld, got %ld\n",
              scenario, i, expected[i], s_backoff_delays[i]);
      verdict = 1;
    }
  }

  return verdict;
}

int
main(int argc, char **argv)
{
  const char *host   = "127.0.0.1";
  const char *label  = "uflib-test";
  unsigned int port  = 63790;
  const char *expect = "exhaustion";

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--host") == 0 && i + 1 < argc)    host   = argv[++i];
    else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = (unsigned int)strtoul(argv[++i], NULL, 10);
    else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) label = argv[++i];
    else if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc) expect = argv[++i];
    else {
      usage(argv[0]);
      return 2;
    }
  }

  if (strcmp(expect, "connect_down") != 0 &&
      strcmp(expect, "exhaustion") != 0 &&
      strcmp(expect, "recovery") != 0 &&
      strcmp(expect, "recovery_depth") != 0 &&
      strcmp(expect, "store_result_loss") != 0) {
    usage(argv[0]);
    return 2;
  }

  PersistenceBackend *per_ptr =
      InitialisePersistenceBackend(NULL, host, port, label, false);

  if (per_ptr == NULL) {
    if (strcmp(expect, "connect_down") == 0) {
      printf("[%lld] CONNECT_FAIL (expected)\n", now_ms());
      fflush(stdout);
      return 0;
    }
    fprintf(stderr, "InitialisePersistenceBackend(%s:%u) failed\n", host, port);
    return 2;
  }

  if (strcmp(expect, "connect_down") == 0) {
    fprintf(stderr, "connect_down: expected connect failure, but connected\n");
    return 1;
  }

  printf("[%lld] READY\n", now_ms());
  fflush(stdout);

  int verdict = 0;

  if (strcmp(expect, "store_result_loss") == 0) {
    /* MULTI/EXEC read back via RedisGetReply — a different reconnect path (obuf
     * save/restore).  Sleep first so the script can arm reset_peer before the
     * first write. */
    sleep(1);

    per_ptr->send_command_multi(NULL, per_ptr, "MULTI");
    per_ptr->send_command_multi(NULL, per_ptr, "SET uflib_test_block foo");
    per_ptr->send_command_multi(NULL, per_ptr, "EXEC");

    enum { N_REPLIES = 3 };
    redisReply *replies[N_REPLIES] = { NULL, NULL, NULL };
    int rc = REDIS_OK;
    int i;
    for (i = 0; i < N_REPLIES; ++i) {
      rc = RedisGetReply(NULL, per_ptr, (void *)&replies[i]);
      if (rc != REDIS_OK) {
        break;
      }
    }
    for (i = 0; i < N_REPLIES; ++i) {
      if (replies[i] != NULL) {
        freeReplyObject(replies[i]);
      }
    }

    printf("[%lld] RESULT getreply_rc=%d backoffs=%zu\n", now_ms(), rc, s_backoff_count);
    fflush(stdout);

    if (rc != REDIS_ERR) {
      fprintf(stderr, "store_result_loss: expected REDIS_ERR (%d), got %d\n", REDIS_ERR, rc);
      verdict = 1;
    }
    const long expected[] = { 1000, 2000, 4000, 8000, 10000 };
    verdict |= sAssertBackoffSequence("store_result_loss", expected,
                                      sizeof(expected) / sizeof(expected[0]));
  } else {
    /* Blocking command via RedisSendCommand: gives the script a window. */
    void *_reply = RedisSendCommand(per_ptr, "BLPOP uflib_test_block 5");

    printf("[%lld] RESULT reply=%s backoffs=%zu\n",
           now_ms(), _reply ? "non-NULL" : "NULL", s_backoff_count);
    fflush(stdout);

    if (_reply != NULL) {
      freeReplyObject((redisReply *)_reply);
    }

    if (strcmp(expect, "exhaustion") == 0) {
      const long expected[] = { 1000, 2000, 4000, 8000, 10000 };
      if (_reply != NULL) {
        fprintf(stderr, "exhaustion: expected NULL reply, got non-NULL\n");
        verdict = 1;
      }
      verdict |= sAssertBackoffSequence("exhaustion", expected,
                                        sizeof(expected) / sizeof(expected[0]));
    } else if (strcmp(expect, "recovery") == 0) {
      if (_reply == NULL) {
        fprintf(stderr, "recovery: expected non-NULL reply, got NULL\n");
        verdict = 1;
      }
      /* Recovery via transient outage: at least one backoff must have elapsed. */
      if (s_backoff_count == 0) {
        fprintf(stderr, "recovery: expected at least one backoff, got none\n");
        verdict = 1;
      }
    } else if (strcmp(expect, "recovery_depth") == 0) {
      const long expected[] = { 1000, 2000, 4000, 8000 };
      if (_reply == NULL) {
        fprintf(stderr, "recovery_depth: expected non-NULL reply, got NULL\n");
        verdict = 1;
      }
      verdict |= sAssertBackoffSequence("recovery_depth", expected,
                                        sizeof(expected) / sizeof(expected[0]));
    }
  }

  return verdict;
}
