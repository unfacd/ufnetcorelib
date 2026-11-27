/**
 * @file ratelimit_unit_driver.c
 * @brief Deterministic adversarial unit tests for the ufnetcorelib ratelimit module.
 *
 * Exercises `GetRequestRateLimitStatus` and its public wrappers WITHOUT a live
 * Redis by substituting two boundaries at link time:
 *
 *   - `--wrap=RedisGetReply`        -> __wrap_RedisGetReply returns canned
 *                                      redisReply trees (the MULTI/EXEC reply
 *                                      contract: OK / QUEUED x4 / EXEC-array).
 *   - `--wrap=GetTimeNowInMicros`   -> __wrap_GetTimeNowInMicros returns a
 *                                      fixed, settable clock.
 *
 * The `send_command_multi` function pointer on the fake CacheBackend records
 * every dispatched command, so the tests can assert the exact Redis command
 * sequence and its arithmetic (clear_before, EXPIRE seconds, ZADD member).
 *
 * Two tiers of tests:
 *   - GREEN (run by default): assertions on correct behaviour that the current
 *     code satisfies.
 *   - `--known-defects`: assertions that CHARACTERISE the current buggy
 *     behaviour of the findings recorded in UFLIB_AUDIT_RATELIMIT.md (H1, H3,
 *     M1, F3, F11, F9, NULL-safety). These pass *while* the defect exists and
 *     will fail once it is fixed — a tripwire, not a behaviour lock.
 *
 * Exit code: number of failed checks (0 = all green).
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
#include <stdarg.h>
#include <pthread.h>

#include <ufnet_core/cache_backend/redis.h>
#include <ufnet_core/ratelimit/ratelimit.h>
#include <ufnet_core/thread_context_type.h>

/* The thread-local the library extern-declares; the app server normally owns it. */
__thread ThreadContext ufsrv_thread_context;

/* ── Linker-wrap boundary state ──────────────────────────────────────────── */

static long long s_now_micros = 0;         /* value returned by GetTimeNowInMicros */
static redisReply *s_replies[6];           /* canned replies, one per RedisGetReply call */
static int         s_reply_index = 0;
static int         s_reply_err_at = -1;    /* index that returns REDIS_ERR (inject failure) */

/* Recorded commands dispatched through send_command_multi. */
static char s_cmds[6][512];
static int  s_cmd_count = 0;

long long
__wrap_GetTimeNowInMicros(void)
{
  return s_now_micros;
}

int
__wrap_RedisGetReply(Session *sesn, CacheBackend *pers, redisReply **reply)
{
  (void)sesn;
  (void)pers;

  if (s_reply_index >= 6) {
    *reply = NULL;
    return REDIS_ERR;
  }
  int i = s_reply_index++;
  *reply = s_replies[i];
  return (i == s_reply_err_at) ? REDIS_ERR : REDIS_OK;
}

/* Recording send_command_multi stub (matches CacheBackend::send_command_multi). */
static void *
sSendCommandMulti(void *sesn, CacheBackend *pers, const char *format, ...)
{
  (void)sesn;
  (void)pers;

  char buf[512];
  va_list ap;
  va_start(ap, format);
  vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);

  if (s_cmd_count < 6) {
    snprintf(s_cmds[s_cmd_count], sizeof(s_cmds[0]), "%s", buf);
    s_cmd_count++;
  }
  return (void *)1;
}

/* ── Minimal check harness ───────────────────────────────────────────────── */

static int s_checks   = 0;
static int s_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    s_checks++;                                                            \
    if (!(cond)) {                                                         \
      s_failures++;                                                        \
      fprintf(stderr, "  FAIL %s:%d: %s\n", __func__, __LINE__, #cond);    \
    }                                                                      \
  } while (0)

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    long long _a = (long long)(a);                                         \
    long long _b = (long long)(b);                                         \
    s_checks++;                                                            \
    if (_a != _b) {                                                        \
      s_failures++;                                                        \
      fprintf(stderr, "  FAIL %s:%d: %s == %s  (got %lld, want %lld)\n",   \
              __func__, __LINE__, #a, #b, _a, _b);                         \
    }                                                                      \
  } while (0)

/* ── redisReply fabrication ──────────────────────────────────────────────── */

static redisReply *
sNewStatus(const char *s)
{
  redisReply *r = calloc(1, sizeof(redisReply));
  r->type = REDIS_REPLY_STATUS;
  r->str  = strdup(s);
  r->len  = strlen(s);
  return r;
}

static redisReply *
sNewError(const char *s)
{
  redisReply *r = calloc(1, sizeof(redisReply));
  r->type = REDIS_REPLY_ERROR;
  r->str  = strdup(s);
  r->len  = strlen(s);
  return r;
}

static redisReply *
sNewInteger(long long v)
{
  redisReply *r = calloc(1, sizeof(redisReply));
  r->type    = REDIS_REPLY_INTEGER;
  r->integer = v;
  return r;
}

static redisReply *
sNewString(const char *s)
{
  redisReply *r = calloc(1, sizeof(redisReply));
  r->type = REDIS_REPLY_STRING;
  r->str  = strdup(s);
  r->len  = strlen(s);
  return r;
}

static redisReply *
sNewArray(size_t n)
{
  redisReply *r = calloc(1, sizeof(redisReply));
  r->type     = REDIS_REPLY_ARRAY;
  r->elements = n;
  r->element  = n ? calloc(n, sizeof(redisReply *)) : NULL;
  return r;
}

/* ZRANGE result: an array of timestamp-member strings. */
static redisReply *
sBuildZrange(const char *const *timestamps, size_t n)
{
  redisReply *zr = sNewArray(n);
  for (size_t i = 0; i < n; i++) {
    zr->element[i] = sNewString(timestamps[i]);
  }
  return zr;
}

/*
 * Build the full 6-reply MULTI/EXEC contract and install it as the canned
 * reply sequence.  `zrange` is adopted into the EXEC array (element index 1).
 */
static void
sSetupMultiReplies(redisReply *zrange)
{
  s_replies[0] = sNewStatus("OK");       /* MULTI             */
  s_replies[1] = sNewStatus("QUEUED");   /* ZREMRANGEBYSCORE  */
  s_replies[2] = sNewStatus("QUEUED");   /* ZRANGE            */
  s_replies[3] = sNewStatus("QUEUED");   /* ZADD              */
  s_replies[4] = sNewStatus("QUEUED");   /* EXPIRE            */

  redisReply *exec = sNewArray(4);       /* EXEC -> 4 elements */
  exec->element[0] = sNewInteger(0);     /* ZREMRANGEBYSCORE result (unused) */
  exec->element[1] = zrange;             /* ZRANGE result (the one that matters) */
  exec->element[2] = sNewInteger(1);     /* ZADD result (unused)               */
  exec->element[3] = sNewInteger(1);     /* EXPIRE result (unused)             */
  s_replies[5] = exec;
}

/* ── Scenario reset ──────────────────────────────────────────────────────── */

static void
sReset(void)
{
  s_now_micros   = 0;
  s_reply_index  = 0;
  s_reply_err_at = -1;
  s_cmd_count    = 0;
  memset(s_cmds, 0, sizeof(s_cmds));
  for (int i = 0; i < 6; i++) {
    s_replies[i] = NULL;
  }
}

/* ── Test scaffolding ────────────────────────────────────────────────────── */

static CacheBackend      s_backend;
static UFSRVResult       s_result;
static RateLimitDescriptor s_desc;
static RequestRateLimit  s_spec;

static void
sInitScaffold(void)
{
  memset(&s_backend, 0, sizeof(s_backend));
  s_backend.send_command_multi = sSendCommandMulti;
  memset(&s_result, 0, sizeof(s_result));

  memset(&s_desc, 0, sizeof(s_desc));
  s_desc.pers_ptr   = &s_backend;
  s_desc.namespace  = "testns";
  s_desc.request_rl_ptr = &s_spec;
  s_desc.res_ptr    = &s_result;
}

static RequestRateLimit
sSpec(size_t interval, size_t max, size_t min_diff)
{
  RequestRateLimit r = {0};
  r.interval         = interval;
  r.max_in_interval  = max;
  r.min_difference   = min_diff;
  return r;
}

/* ── GREEN tests (correct behaviour the current code satisfies) ──────────── */

static void
tCommandSequenceAndArithmetic(void)
{
  sReset();
  sInitScaffold();
  s_spec = sSpec(1000, 20, 0);          /* interval 1000 ms, max 20, no min-diff */
  s_now_micros = 5000000LL;

  const char *zr[] = {};                 /* empty window */
  sSetupMultiReplies(sBuildZrange(zr, 0));

  RequestRateLimitStatus st;
  UFSRVResult *res = GetRequestRateLimitStatus(&s_desc, &st);

  CHECK(res == &s_result);
  CHECK_EQ(s_result.result_type, RESULT_TYPE_SUCCESS);
  CHECK_EQ(s_result.result_code, RESCODE_BACKEND_DATA_SETCREATED);
  CHECK(s_result.result_user_data == (void *)&st);

  CHECK_EQ(s_cmd_count, 6);
  CHECK(strcmp(s_cmds[0], "MULTI") == 0);
  CHECK(strstr(s_cmds[1], "ZREMRANGEBYSCORE") != NULL);
  CHECK(strstr(s_cmds[1], "testns") != NULL);
  CHECK(strstr(s_cmds[1], "4000000") != NULL);   /* clear_before = now - 1000ms */
  CHECK(strstr(s_cmds[2], "ZRANGE testns 0 -1") != NULL);
  CHECK(strstr(s_cmds[3], "ZADD testns 5000000 5000000") != NULL);
  CHECK(strstr(s_cmds[4], "EXPIRE testns 1") != NULL);  /* (1000*1000)/1000000 = 1 s */
  CHECK(strcmp(s_cmds[5], "EXEC") == 0);

  /* Empty window: remaining = max - 1 (this request consumed one). */
  CHECK_EQ(st.remaining_actions, 19);
  CHECK_EQ(st.remaining_time, 0);
}

static void
tEmptySet(void)
{
  sReset();
  sInitScaffold();
  s_spec = sSpec(1000, 1, 100);         /* RLNS_CONNECTONS-shaped */
  s_now_micros = 9000000LL;

  const char *zr[] = {};
  sSetupMultiReplies(sBuildZrange(zr, 0));

  RequestRateLimitStatus st;
  UFSRVResult *res = GetRequestRateLimitStatus(&s_desc, &st);

  CHECK(res == &s_result);
  CHECK_EQ(s_result.result_type, RESULT_TYPE_SUCCESS);
  CHECK_EQ(st.remaining_actions, 0);    /* max - 1 = 0 */
  CHECK_EQ(st.remaining_time, 0);
}

static void
tExceededByCount(void)
{
  sReset();
  sInitScaffold();
  s_spec = sSpec(1000, 2, 0);           /* max 2, no min-diff */
  s_now_micros = 5000000LL;

  const char *zr[] = {"4200000", "4600000"};   /* 2 prior in window */
  sSetupMultiReplies(sBuildZrange(zr, 2));

  RequestRateLimitStatus st;
  UFSRVResult *res = GetRequestRateLimitStatus(&s_desc, &st);

  CHECK(res == &s_result);
  CHECK_EQ(s_result.result_type, RESULT_TYPE_SUCCESS);
  CHECK_EQ(st.remaining_actions, -1);   /* exceeded -> negative */
  /* v1 = (4200000-5000000)+1000000 = 200000 us -> 200 ms */
  CHECK_EQ(st.remaining_time, 200);
}

static void
tExceededByMinDifference(void)
{
  sReset();
  sInitScaffold();
  s_spec = sSpec(1000, 20, 100);        /* min-diff 100 ms */
  s_now_micros = 5000000LL;

  const char *zr[] = {"4950000"};       /* 50 ms ago (< min-diff) */
  sSetupMultiReplies(sBuildZrange(zr, 1));

  RequestRateLimitStatus st;
  UFSRVResult *res = GetRequestRateLimitStatus(&s_desc, &st);

  CHECK(res == &s_result);
  CHECK_EQ(st.remaining_actions, -1);
  /* v1 = (4950000-5000000)+1000000 = 950000; min-diff term = 100000-50000 = 50000 -> 50 ms */
  CHECK_EQ(st.remaining_time, 50);
}

static void
tNotExceededNonEmpty(void)
{
  sReset();
  sInitScaffold();
  s_spec = sSpec(1000, 20, 0);
  s_now_micros = 5000000LL;

  const char *zr[] = {"4000000", "4500000", "4900000"};  /* 3 prior, well below max */
  sSetupMultiReplies(sBuildZrange(zr, 3));

  RequestRateLimitStatus st;
  UFSRVResult *res = GetRequestRateLimitStatus(&s_desc, &st);

  CHECK(res == &s_result);
  CHECK_EQ(st.remaining_time, 0);
  /* Correct: 3 prior + this request = 4 used, 20 - 4 = 16 remain. */
  CHECK_EQ(st.remaining_actions, 16);
}

static void
tGetRateLimitSpecsForValid(void)
{
  sReset();
  sInitScaffold();

  const RequestRateLimit *c = GetRateLimitSpecsFor(RLNS_CONNECTONS);
  CHECK(c != NULL);
  CHECK_EQ(c->interval, 1000);
  CHECK_EQ(c->max_in_interval, 1);
  CHECK_EQ(c->min_difference, 100);

  const RequestRateLimit *r = GetRateLimitSpecsFor(RLNS_REQUESTS);
  CHECK(r != NULL);
  CHECK_EQ(r->interval, 1000);
  CHECK_EQ(r->max_in_interval, 20);
  CHECK_EQ(r->min_difference, 0);
}

/* H2 (fixed): out-of-range categories return NULL instead of reading OOB. */
static void
tGetRateLimitSpecsForInvalid(void)
{
  CHECK(GetRateLimitSpecsFor(RLNS_MAXVALUE) == NULL);
  CHECK(GetRateLimitSpecsFor((enum RateLimitNamespaceCategory)0) == NULL);
  CHECK(GetRateLimitSpecsFor((enum RateLimitNamespaceCategory)-1) == NULL);
}

static void
tIsRateLimitExceededForSessionNamespace(void)
{
  sReset();
  sInitScaffold();

  /* IsRateLimitExceededForSession reads the result from the thread context. */
  UFSRVResult tcres;
  memset(&tcres, 0, sizeof(tcres));
  ufsrv_thread_context.res_ptr = &tcres;

  s_now_micros = 5000000LL;
  const char *zr[] = {};
  sSetupMultiReplies(sBuildZrange(zr, 0));

  /* RLNS_CONNECTONS: empty window -> not exceeded. */
  bool exceeded = IsRateLimitExceededForSession(&s_backend, 42, 7, RLNS_CONNECTONS);

  CHECK(exceeded == false);
  /* Namespace must be "<uid>:<cid>:<category>" with category as its int value. */
  CHECK(strstr(s_cmds[2], "42:7:1") != NULL);
}

static void
tIsRateLimitExceededForIPAddress(void)
{
  sReset();
  sInitScaffold();
  s_now_micros = 5000000LL;

  const char *zr[] = {};
  sSetupMultiReplies(sBuildZrange(zr, 0));

  /* CONNECTONS: empty window -> not exceeded, and namespace == the IP string. */
  bool exceeded = IsRateLimitExceededForIPAddress(&s_backend, "10.1.2.3", RLNS_CONNECTONS, &s_result);
  CHECK(exceeded == false);
  CHECK(strstr(s_cmds[2], "10.1.2.3") != NULL);
}

/* ── Fixed-defect regression tests (assert the corrected behaviour) ─────── */

/* H1 (fixed): for min_difference == 0 (RLNS_REQUESTS), a full window must now
 * report "exceeded" — remaining_actions is negative, not 0, when exceeded. */
static void
tRequestsExceededBlocks(void)
{
  sReset();
  sInitScaffold();
  s_now_micros = 5000000LL;

  /* 20 prior requests == max_in_interval for RLNS_REQUESTS -> exceeded. */
  static const char *zr[20] = {
    "4000000","4050000","4100000","4150000","4200000","4250000","4300000",
    "4350000","4400000","4450000","4500000","4550000","4600000","4650000",
    "4700000","4750000","4800000","4850000","4900000","4950000",
  };
  sSetupMultiReplies(sBuildZrange(zr, 20));

  bool exceeded = IsRateLimitExceededWithNamespace(&s_desc, RLNS_REQUESTS);
  CHECK(exceeded == true);
}

/* Not a defect (was investigated as a candidate "remaining_time wrap" and
 * disproven): when IsTooManyInInterval is true but the last request is further
 * back than min_difference, the unsigned subtraction wraps to a large positive,
 * so _min correctly returns v1 — the real "time until the window slides". */
static void
tTooManyInIntervalButMinDiffSatisfied(void)
{
  sReset();
  sInitScaffold();
  s_spec = sSpec(1000, 1, 100);         /* max 1, min-diff 100 ms */
  s_now_micros = 5000000LL;

  const char *zr[] = {"4500000"};       /* 500 ms ago: >= min-diff, 1 prior >= max */
  sSetupMultiReplies(sBuildZrange(zr, 1));

  RequestRateLimitStatus st;
  UFSRVResult *res = GetRequestRateLimitStatus(&s_desc, &st);

  CHECK(res == &s_result);
  CHECK_EQ(st.remaining_actions, -1);   /* exceeded by count */
  /* v1 = (4500000-5000000)+1000000 = 500000 us -> 500 ms until the window slides. */
  CHECK_EQ(st.remaining_time, 500);
}

/* L1 (fixed): remaining_actions accounts for the in-flight request. */
static void
tRemainingActionsAccountsForInflight(void)
{
  sReset();
  sInitScaffold();
  s_spec = sSpec(1000, 20, 0);
  s_now_micros = 5000000LL;

  const char *zr[] = {"4000000","4500000","4900000"};  /* 3 prior */
  sSetupMultiReplies(sBuildZrange(zr, 3));

  RequestRateLimitStatus st;
  GetRequestRateLimitStatus(&s_desc, &st);

  /* 3 prior + this request = 4 used, 20 - 4 = 16 remain. */
  CHECK_EQ(st.remaining_actions, 16);
}

/* M1 (fixed): a Redis/backend error returns RESCODE_BACKEND_RESOURCE_NULL. */
static void
tErrorReturnsBackendResourceNull(void)
{
  sReset();
  sInitScaffold();
  s_spec = sSpec(1000, 20, 0);
  s_now_micros = 5000000LL;

  const char *zr[] = {};
  sSetupMultiReplies(sBuildZrange(zr, 0));
  s_reply_err_at = 3;                   /* force the ZADD reply to fail */

  RequestRateLimitStatus st;
  UFSRVResult *res = GetRequestRateLimitStatus(&s_desc, &st);

  CHECK_EQ(res->result_type, RESULT_TYPE_ERR);
  CHECK_EQ(res->result_code, RESCODE_BACKEND_RESOURCE_NULL);
}

/* L4 (fixed): a WRONGTYPE (non-array ZRANGE element) now surfaces an error. */
static void
tWrongTypeSurfacesError(void)
{
  sReset();
  sInitScaffold();
  s_spec = sSpec(1000, 20, 0);
  s_now_micros = 5000000LL;

  /* Build the MULTI/EXEC sequence manually with an ERROR reply as the ZRANGE. */
  s_replies[0] = sNewStatus("OK");
  s_replies[1] = sNewStatus("QUEUED");
  s_replies[2] = sNewStatus("QUEUED");
  s_replies[3] = sNewStatus("QUEUED");
  s_replies[4] = sNewStatus("QUEUED");
  redisReply *exec = sNewArray(4);
  exec->element[0] = sNewInteger(0);
  exec->element[1] = sNewError("WRONGTYPE Operation against a key holding the wrong kind of value");
  exec->element[2] = sNewInteger(1);
  exec->element[3] = sNewInteger(1);
  s_replies[5] = exec;

  RequestRateLimitStatus st;
  UFSRVResult *res = GetRequestRateLimitStatus(&s_desc, &st);

  CHECK_EQ(res->result_type, RESULT_TYPE_ERR);
}

/* L2 (fixed): a sub-second interval rounds the TTL up to at least 1 second. */
static void
tSubSecondIntervalExpiresOneSecond(void)
{
  sReset();
  sInitScaffold();
  s_spec = sSpec(500, 20, 0);           /* interval 500 ms */
  s_now_micros = 5000000LL;

  const char *zr[] = {};
  sSetupMultiReplies(sBuildZrange(zr, 0));

  RequestRateLimitStatus st;
  GetRequestRateLimitStatus(&s_desc, &st);

  CHECK(strstr(s_cmds[4], "EXPIRE testns 1") != NULL);
}

/* M4 (fixed): public entry points no longer crash on NULL. */

static void
tNullDescriptorReturnsNull(void)
{
  sReset();
  sInitScaffold();
  CHECK(GetRequestRateLimitStatus(NULL, NULL) == NULL);
}

static void
tNullRequestRlPtrReturnsError(void)
{
  sReset();
  sInitScaffold();
  s_spec = sSpec(1000, 20, 0);
  s_desc.request_rl_ptr = NULL;
  RequestRateLimitStatus st;
  UFSRVResult *res = GetRequestRateLimitStatus(&s_desc, &st);
  CHECK(res == &s_result);
  CHECK_EQ(s_result.result_type, RESULT_TYPE_ERR);
}

static void
tNullResPtrReturnsNull(void)
{
  sReset();
  sInitScaffold();
  s_spec = sSpec(1000, 20, 0);
  s_desc.res_ptr = NULL;
  RequestRateLimitStatus st;
  CHECK(GetRequestRateLimitStatus(&s_desc, &st) == NULL);
}

static void
tNullNamespaceReturnsError(void)
{
  sReset();
  sInitScaffold();
  s_spec = sSpec(1000, 20, 0);
  s_desc.namespace = NULL;
  RequestRateLimitStatus st;
  UFSRVResult *res = GetRequestRateLimitStatus(&s_desc, &st);
  CHECK(res == &s_result);
  CHECK_EQ(s_result.result_type, RESULT_TYPE_ERR);
}

/* M2 (fixed): a namespace with whitespace is rejected rather than injected. */
static void
tInvalidNamespaceReturnsError(void)
{
  sReset();
  sInitScaffold();
  s_spec = sSpec(1000, 20, 0);
  s_desc.namespace = "bad ns with space";
  RequestRateLimitStatus st;
  UFSRVResult *res = GetRequestRateLimitStatus(&s_desc, &st);
  CHECK(res == &s_result);
  CHECK_EQ(s_result.result_type, RESULT_TYPE_ERR);
  CHECK_EQ(s_result.result_code, RESCODE_PROG_MISSING_PARAM);
}

/* ── Runner ──────────────────────────────────────────────────────────────── */

typedef void (*sTest)(void);
typedef struct { const char *name; sTest fn; } sCase;

int
main(void)
{
  const sCase tests[] = {
    {"command sequence + arithmetic",        tCommandSequenceAndArithmetic},
    {"empty set -> max-1",                  tEmptySet},
    {"exceeded by count",                   tExceededByCount},
    {"exceeded by min_difference",          tExceededByMinDifference},
    {"too many but min-diff satisfied",     tTooManyInIntervalButMinDiffSatisfied},
    {"not exceeded (non-empty)",            tNotExceededNonEmpty},
    {"GetRateLimitSpecsFor valid",          tGetRateLimitSpecsForValid},
    {"GetRateLimitSpecsFor invalid",        tGetRateLimitSpecsForInvalid},
    {"session namespace construction",      tIsRateLimitExceededForSessionNamespace},
    {"IP-address namespace",                tIsRateLimitExceededForIPAddress},
    {"requests exceeded blocks (H1)",       tRequestsExceededBlocks},
    {"remaining actions account in-flight (L1)", tRemainingActionsAccountsForInflight},
    {"error returns backend resource null (M1)", tErrorReturnsBackendResourceNull},
    {"wrongtype surfaces error (L4)",       tWrongTypeSurfacesError},
    {"sub-second interval EXPIRE 1 (L2)",   tSubSecondIntervalExpiresOneSecond},
    {"NULL descriptor -> NULL (M4)",        tNullDescriptorReturnsNull},
    {"NULL request_rl_ptr -> error (M4)",   tNullRequestRlPtrReturnsError},
    {"NULL res_ptr -> NULL (M4)",           tNullResPtrReturnsNull},
    {"NULL namespace -> error (M4)",        tNullNamespaceReturnsError},
    {"whitespace namespace -> error (M2)",  tInvalidNamespaceReturnsError},
  };

  printf("ratelimit_unit_driver: tests\n");
  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    int before = s_failures;
    tests[i].fn();
    printf("  [%s] %s\n", s_failures == before ? "PASS" : "FAIL", tests[i].name);
  }

  printf("ratelimit_unit_driver: %d checks, %d failures\n", s_checks, s_failures);
  return s_failures == 0 ? 0 : 1;
}
