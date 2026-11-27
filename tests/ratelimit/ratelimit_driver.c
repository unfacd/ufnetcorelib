/**
 * @file ratelimit_driver.c
 * @brief Standalone live-Redis integration driver for the ufnetcorelib ratelimit module.
 *
 * Links the *production* translation units (ratelimit.c + redis.c +
 * persistence.c + vendored hiredis + system uflib) and drives the rate limiter
 * the way a real consumer does, against a scratch Redis (optionally fronted by
 * Toxiproxy). This exercises the distributed sliding-window path byte for byte.
 *
 * Scenarios (--scenario):
 *   single       — one connection, spaced requests; asserts the ZSET window
 *                  enforces the limit (remaining_actions falls monotonically to 0).
 *   concurrency  — N threads, each with its OWN connection, barrier-synchronised
 *                  onto one namespace; reports how many distinct timestamps the
 *                  ZSET actually holds (see finding F5: same-microsecond members
 *                  collapse).
 *   collapse     — same as concurrency but with GetTimeNowInMicros wrapped to a
 *                  FIXED clock, so every thread ZADDs an identical *score*; the
 *                  unique-member nonce must keep them distinct — asserts the ZSET
 *                  cardinality is N (regression proof for M3).
 *   connect_down — endpoint down; InitialisePersistenceBackend returns NULL.
 *
 * The thread-local `ufsrv_thread_context` the library only extern-declares is
 * supplied here (its definition normally lives in the app server).
 *
 * Usage:
 *   ratelimit_driver --host 127.0.0.1 --port 63794 --label uflib-rl \
 *       --scenario single [--threads 32]
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
#include <pthread.h>
#include <unistd.h>

#include <ufnet_core/cache_backend/redis.h>
#include <ufnet_core/cache_backend/persistence.h>
#include <ufnet_core/ratelimit/ratelimit.h>
#include <ufnet_core/thread_context_type.h>

/* The thread-local the library extern-declares; the app server normally owns it. */
__thread ThreadContext ufsrv_thread_context;

/*
 * Fixed-clock override for the `collapse` scenario: with a constant timestamp,
 * every concurrent ZADD uses an identical member, deterministically exposing
 * finding F5 (identical members collapse).  When off, delegate to the real clock.
 */
static long long s_fixed_clock     = 0;
static int      s_use_fixed_clock  = 0;

long long __real_GetTimeNowInMicros(void);

long long
__wrap_GetTimeNowInMicros(void)
{
  return s_use_fixed_clock ? s_fixed_clock : __real_GetTimeNowInMicros();
}

static int
sDel(PersistenceBackend *per, const char *key)
{
  void *r = RedisSendCommand(per, "DEL %s", key);
  if (r) {
    freeReplyObject((redisReply *)r);
  }
  return 0;
}

/* ── single ──────────────────────────────────────────────────────────────── */

static int
sRunSingle(const char *host, unsigned port, const char *label)
{
  PersistenceBackend *per = InitialisePersistenceBackend(NULL, host, port, label, false);
  if (per == NULL) {
    fprintf(stderr, "single: InitialisePersistenceBackend(%s:%u) failed\n", host, port);
    return 1;
  }
  sDel(per, "rl_single");

  RequestRateLimit spec = { .interval = 1000, .max_in_interval = 5, .min_difference = 0 };
  UFSRVResult res;
  memset(&res, 0, sizeof(res));
  RateLimitDescriptor desc = {
    .pers_ptr = per, .namespace = "rl_single",
    .request_rl_ptr = &spec, .res_ptr = &res,
  };

  int     verdict     = 0;
  ssize_t prev        = (ssize_t)1 << 20;
  int     reached_zero = 0;
  int     calls       = 9;

  for (int i = 0; i < calls; i++) {
    RequestRateLimitStatus st;
    UFSRVResult *r = GetRequestRateLimitStatus(&desc, &st);

    if (r == NULL || res.result_type != RESULT_TYPE_SUCCESS) {
      fprintf(stderr, "single: call %d returned an error result\n", i);
      verdict = 1;
      break;
    }
    if (i == 0 && st.remaining_actions != (ssize_t)(spec.max_in_interval - 1)) {
      fprintf(stderr, "single: first call remaining_actions=%zd, want %zu\n",
              st.remaining_actions, spec.max_in_interval - 1);
      verdict = 1;
    }
    if (st.remaining_actions > prev) {
      fprintf(stderr, "single: remaining_actions rose from %zd to %zd\n", prev, st.remaining_actions);
      verdict = 1;
    }
    prev = st.remaining_actions;
    if (st.remaining_actions == 0) {
      reached_zero = 1;
    }
    printf("[single] call=%d remaining_actions=%zd remaining_time=%zu\n",
           i, st.remaining_actions, st.remaining_time);
    fflush(stdout);
    usleep(1500);  /* distinct timestamps: 1.5 ms apart, well within the 1 s window */
  }

  if (!reached_zero) {
    fprintf(stderr, "single: limit never reached (remaining_actions stayed > 0)\n");
    verdict = 1;
  }

  return verdict;
}

/* ── concurrency / collapse (shared worker) ──────────────────────────────── */

typedef struct {
  const char          *host;
  unsigned             port;
  const char          *label;
  const char          *ns;
  const RequestRateLimit *spec;
  int                  failed;
} sConcCtx;

static pthread_barrier_t s_barrier;

static void *
sConcWorker(void *arg)
{
  sConcCtx *c = (sConcCtx *)arg;

  PersistenceBackend *per = InitialisePersistenceBackend(NULL, c->host, c->port, c->label, false);
  if (per == NULL) {
    c->failed = 1;
    return NULL;
  }

  UFSRVResult res;
  memset(&res, 0, sizeof(res));
  RateLimitDescriptor desc = {
    .pers_ptr = per, .namespace = c->ns,
    .request_rl_ptr = c->spec, .res_ptr = &res,
  };

  pthread_barrier_wait(&s_barrier);

  RequestRateLimitStatus st;
  UFSRVResult *r = GetRequestRateLimitStatus(&desc, &st);
  if (r == NULL || res.result_type != RESULT_TYPE_SUCCESS) {
    c->failed = 1;
  }

  return NULL;
}

/* max_in_interval is deliberately huge so every call just ZADDs its member
 * without ever entering the "exceeded" branch — we only want to count members. */
static int
sRunConcurrent(const char *host, unsigned port, const char *label,
               const char *ns, int nthreads, int fixed_clock, int expect_unique)
{
  PersistenceBackend *main_per = InitialisePersistenceBackend(NULL, host, port, label, false);
  if (main_per == NULL) {
    fprintf(stderr, "%s: InitialisePersistenceBackend(%s:%u) failed\n", ns, host, port);
    return 1;
  }
  sDel(main_per, ns);

  RequestRateLimit spec = { .interval = 1000, .max_in_interval = 1000000, .min_difference = 0 };

  if (fixed_clock) {
    s_fixed_clock    = 1234567890123LL;
    s_use_fixed_clock = 1;
  }

  pthread_barrier_init(&s_barrier, NULL, (unsigned)nthreads);
  pthread_t *tids = calloc((size_t)nthreads, sizeof(pthread_t));
  sConcCtx  *ctxs = calloc((size_t)nthreads, sizeof(sConcCtx));

  for (int i = 0; i < nthreads; i++) {
    ctxs[i].host  = host;
    ctxs[i].port  = port;
    ctxs[i].label = label;
    ctxs[i].ns    = ns;
    ctxs[i].spec  = &spec;
    ctxs[i].failed = 0;
    pthread_create(&tids[i], NULL, sConcWorker, &ctxs[i]);
  }
  for (int i = 0; i < nthreads; i++) {
    pthread_join(tids[i], NULL);
  }
  pthread_barrier_destroy(&s_barrier);

  s_use_fixed_clock = 0;

  redisReply *r = (redisReply *)RedisSendCommand(main_per, "ZCARD %s", ns);
  long long distinct = (r != NULL && r->type == REDIS_REPLY_INTEGER) ? r->integer : -1;
  if (r != NULL) {
    freeReplyObject(r);
  }

  int failed = 0;
  for (int i = 0; i < nthreads; i++) {
    if (ctxs[i].failed) {
      failed = 1;
    }
  }

  long long collapsed = (long long)nthreads - distinct;
  printf("[%s] TOTAL=%d DISTINCT=%lld COLLAPSED=%lld\n",
         ns, nthreads, distinct, collapsed);
  fflush(stdout);

  free(tids);
  free(ctxs);

  if (failed) {
    fprintf(stderr, "%s: at least one worker failed\n", ns);
    return 1;
  }
  if (expect_unique && distinct != nthreads) {
    fprintf(stderr, "%s: expected ZCARD==%d (unique members), got %lld\n", ns, nthreads, distinct);
    return 1;
  }
  return 0;
}

/* ── connect_down ────────────────────────────────────────────────────────── */

static int
sRunConnectDown(const char *host, unsigned port, const char *label)
{
  PersistenceBackend *per = InitialisePersistenceBackend(NULL, host, port, label, false);
  if (per == NULL) {
    printf("[connect_down] CONNECT_FAIL (expected)\n");
    fflush(stdout);
    return 0;
  }
  fprintf(stderr, "connect_down: expected connect failure, but connected\n");
  return 1;
}

/* ── runner ──────────────────────────────────────────────────────────────── */

static void
usage(const char *prog)
{
  fprintf(stderr,
          "usage: %s --host H --port P [--label L] "
          "--scenario single|concurrency|collapse|connect_down [--threads N]\n",
          prog);
}

int
main(int argc, char **argv)
{
  const char *host     = "127.0.0.1";
  const char *label    = "uflib-rl";
  unsigned    port     = 63794;
  const char *scenario = "single";
  int         nthreads = 32;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--host") == 0 && i + 1 < argc)        host = argv[++i];
    else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)   port = (unsigned)strtoul(argv[++i], NULL, 10);
    else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc)  label = argv[++i];
    else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) scenario = argv[++i];
    else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) nthreads = (int)strtol(argv[++i], NULL, 10);
    else { usage(argv[0]); return 2; }
  }

  if (strcmp(scenario, "single") == 0) {
    return sRunSingle(host, port, label);
  }
  if (strcmp(scenario, "concurrency") == 0) {
    return sRunConcurrent(host, port, label, "rl_conc", nthreads, 0, 0);
  }
  if (strcmp(scenario, "collapse") == 0) {
    return sRunConcurrent(host, port, label, "rl_collapse", nthreads, 1, 1);
  }
  if (strcmp(scenario, "connect_down") == 0) {
    return sRunConnectDown(host, port, label);
  }

  usage(argv[0]);
  return 2;
}
