/**
 * @file ratelimit_v2_unit_driver.c
 * @brief Deterministic unit tests for the ratelimit_v2 Redis backend + core.
 *
 * Exercises the Redis backend WITHOUT a live Redis by injecting the three hooks
 * (send_command_multi / get_reply / free_reply) through RateLimitRedisHooks and
 * overriding the weak RateLimitNowInMicros with a strong fixed-clock
 * definition.  The fake send_command_multi records both the format string
 * (the argv-structure
 * tripwire) and the fully formatted command; get_reply serves canned redisReply
 * trees matching the MULTI/EXEC contract.
 *
 * Exit code: number of failed checks (0 = all green).
 *
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

#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>

#include "hiredis/hiredis.h"   /* redisReply, REDIS_REPLY_*, REDIS_OK/ERR, freeReplyObject */
#include <ufsrv_core/ratelimit_v2/ratelimit.h>

/* ── Fixed clock (linker wrap) ───────────────────────────────────────────── */

static long long sNowMicros = 0;

/* Strong override of the weak RateLimitNowInMicros defined in ratelimit.c. */
long long
RateLimitNowInMicros(void)
{
  return sNowMicros;
}

/* ── Fake hooks ──────────────────────────────────────────────────────────── */

/* The format string is recorded verbatim: the Redis backend must pass a
 * multi-token format ("ZADD %s %lld %s", ...) — not a pre-built command as a
 * single "%s" argument, which hiredis would collapse to one argv element. */
static char sCmdFmts[6][128];
static char sCmds[6][512];
static int  sCmdCount = 0;

static redisReply *sReplies[6];
static int         sReplyIndex = 0;
static int         sReplyErrAt = -1;   /* index that returns REDIS_ERR */

static void *
sFakeSendMulti(void *ctx, CacheBackend *pers, const char *format, ...)
{
  (void)ctx;
  (void)pers;

  char buf[512];
  va_list ap;
  va_start(ap, format);
  vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);

  if (sCmdCount < 6) {
    snprintf(sCmdFmts[sCmdCount], sizeof(sCmdFmts[0]), "%s", format);
    snprintf(sCmds[sCmdCount], sizeof(sCmds[0]), "%s", buf);
    sCmdCount++;
  }
  return (void *)1;
}

static int
sFakeGetReply(Session *ctx, CacheBackend *pers, redisReply **reply)
{
  (void)ctx;
  (void)pers;

  if (sReplyIndex >= 6) {
    *reply = NULL;
    return REDIS_ERR;
  }
  int i = sReplyIndex++;
  *reply = sReplies[i];
  return (i == sReplyErrAt) ? REDIS_ERR : REDIS_OK;
}

/* ── Minimal check harness ───────────────────────────────────────────────── */

static int sChecks   = 0;
static int sFailures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    sChecks++;                                                            \
    if (!(cond)) {                                                         \
      sFailures++;                                                        \
      fprintf(stderr, "  FAIL %s:%d: %s\n", __func__, __LINE__, #cond);    \
    }                                                                      \
  } while (0)

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    long long _a = (long long)(a);                                         \
    long long _b = (long long)(b);                                         \
    sChecks++;                                                            \
    if (_a != _b) {                                                        \
      sFailures++;                                                        \
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
  for (size_t i = 0; i < n; i++)
    zr->element[i] = sNewString(timestamps[i]);
  return zr;
}

/*
 * Build the full 6-reply MULTI/EXEC contract and install it as the canned
 * reply sequence.  `zrange` is adopted into the EXEC array (element index 1).
 */
static void
sSetupMultiReplies(redisReply *zrange)
{
  sReplies[0] = sNewStatus("OK");       /* MULTI             */
  sReplies[1] = sNewStatus("QUEUED");   /* ZREMRANGEBYSCORE  */
  sReplies[2] = sNewStatus("QUEUED");   /* ZRANGE            */
  sReplies[3] = sNewStatus("QUEUED");   /* ZADD              */
  sReplies[4] = sNewStatus("QUEUED");   /* EXPIRE            */

  redisReply *exec = sNewArray(4);       /* EXEC -> 4 elements */
  exec->element[0] = sNewInteger(0);     /* ZREMRANGEBYSCORE result (unused) */
  exec->element[1] = zrange;             /* ZRANGE result (the one that matters) */
  exec->element[2] = sNewInteger(1);     /* ZADD result (unused)               */
  exec->element[3] = sNewInteger(1);     /* EXPIRE result (unused)             */
  sReplies[5] = exec;
}

/* ── Scenario reset ──────────────────────────────────────────────────────── */

static void
sReset(void)
{
  sNowMicros   = 0;
  sReplyIndex  = 0;
  sReplyErrAt = -1;
  sCmdCount    = 0;
  memset(sCmdFmts, 0, sizeof(sCmdFmts));
  memset(sCmds, 0, sizeof(sCmds));
  for (int i = 0; i < 6; i++)
    sReplies[i] = NULL;
}

/* ── Scaffolding ─────────────────────────────────────────────────────────── */

static RateLimitRedisHooks sHooks;
static struct RateLimitBackend *sBe;
static RateLimitHandle   sH;
static RateLimitResult   sResult;
static RateLimitPolicy  sTestSpec;

static void
sInitScaffold(void)
{
  memset(&sResult, 0, sizeof(sResult));
  sH.backend = sBe;
  sH.result  = &sResult;
}

static RateLimitPolicy
sSpec(size_t interval, size_t max, size_t min_diff)
{
  RateLimitPolicy r = {0};
  r.interval        = interval;
  r.max_in_interval = max;
  r.min_difference  = min_diff;
  return r;
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

static void
tCommandSequenceAndArithmetic(void)
{
  sReset();
  sInitScaffold();
  sTestSpec = sSpec(1000, 20, 0);          /* interval 1000 ms, max 20, no min-diff */
  sNowMicros = 5000000LL;

  const char *zr[] = {};                 /* empty window */
  sSetupMultiReplies(sBuildZrange(zr, 0));

  RateLimitStatus st;
  enum RateLimitResultCode rc = RateLimitCheck(&sH, "testns", &sTestSpec, &st);

  CHECK(rc == RL_OK);
  CHECK_EQ(sResult.code, RL_OK);
  CHECK(sResult.user_data == (void *)&st);

  CHECK_EQ(sCmdCount, 6);
  /* argv-structure tripwire: the backend must pass multi-token formats. */
  CHECK(strcmp(sCmdFmts[0], "MULTI") == 0);
  CHECK(strcmp(sCmdFmts[1], "ZREMRANGEBYSCORE %s:%s 0 %lld") == 0);
  CHECK(strcmp(sCmdFmts[2], "ZRANGE %s:%s 0 -1") == 0);
  CHECK(strcmp(sCmdFmts[3], "ZADD %s:%s %lld %s") == 0);
  CHECK(strcmp(sCmdFmts[4], "EXPIRE %s:%s %lu") == 0);
  CHECK(strcmp(sCmdFmts[5], "EXEC") == 0);

  /* Fully formatted command content. */
  CHECK(strcmp(sCmds[0], "MULTI") == 0);
  CHECK(strstr(sCmds[1], "ZREMRANGEBYSCORE") != NULL);
  CHECK(strstr(sCmds[1], "_RL:testns") != NULL);
  CHECK(strstr(sCmds[1], "4000000") != NULL);   /* clear_before = now - 1000ms */
  CHECK(strstr(sCmds[2], "ZRANGE _RL:testns 0 -1") != NULL);
  CHECK(strstr(sCmds[3], "ZADD _RL:testns 5000000 5000000") != NULL);
  CHECK(strstr(sCmds[4], "EXPIRE _RL:testns 1") != NULL);  /* (1000*1000)/1000000 = 1 s */
  CHECK(strcmp(sCmds[5], "EXEC") == 0);

  /* Empty window: remaining = max - 1 (this request consumed one). */
  CHECK_EQ(st.remaining_actions, 19);
  CHECK_EQ(st.remaining_time_ms, 0);
}

static void
tEmptySet(void)
{
  sReset();
  sInitScaffold();
  sTestSpec = sSpec(1000, 1, 100);         /* RL_CAT_CONNECTONS-shaped */
  sNowMicros = 9000000LL;

  const char *zr[] = {};
  sSetupMultiReplies(sBuildZrange(zr, 0));

  RateLimitStatus st;
  enum RateLimitResultCode rc = RateLimitCheck(&sH, "testns", &sTestSpec, &st);

  CHECK(rc == RL_OK);
  CHECK_EQ(sResult.code, RL_OK);
  CHECK_EQ(st.remaining_actions, 0);    /* max - 1 = 0 */
  CHECK_EQ(st.remaining_time_ms, 0);
}

static void
tExceededByCount(void)
{
  sReset();
  sInitScaffold();
  sTestSpec = sSpec(1000, 2, 0);           /* max 2, no min-diff */
  sNowMicros = 5000000LL;

  const char *zr[] = {"4200000", "4600000"};   /* 2 prior in window */
  sSetupMultiReplies(sBuildZrange(zr, 2));

  RateLimitStatus st;
  enum RateLimitResultCode rc = RateLimitCheck(&sH, "testns", &sTestSpec, &st);

  CHECK(rc == RL_OK);
  CHECK_EQ(st.remaining_actions, -1);   /* exceeded -> negative */
  /* v1 = (4200000-5000000)+1000000 = 200000 us -> 200 ms */
  CHECK_EQ(st.remaining_time_ms, 200);
}

static void
tExceededByMinDifference(void)
{
  sReset();
  sInitScaffold();
  sTestSpec = sSpec(1000, 20, 100);        /* min-diff 100 ms */
  sNowMicros = 5000000LL;

  const char *zr[] = {"4950000"};       /* 50 ms ago (< min-diff) */
  sSetupMultiReplies(sBuildZrange(zr, 1));

  RateLimitStatus st;
  enum RateLimitResultCode rc = RateLimitCheck(&sH, "testns", &sTestSpec, &st);

  CHECK(rc == RL_OK);
  CHECK_EQ(st.remaining_actions, -1);
  /* v1 = (4950000-5000000)+1000000 = 950000; min-diff term = 100000-50000 = 50000 -> 50 ms */
  CHECK_EQ(st.remaining_time_ms, 50);
}

static void
tTooManyButMinDiffSatisfied(void)
{
  sReset();
  sInitScaffold();
  sTestSpec = sSpec(1000, 1, 100);         /* max 1, min-diff 100 ms */
  sNowMicros = 5000000LL;

  const char *zr[] = {"4500000"};       /* 500 ms ago: >= min-diff, 1 prior >= max */
  sSetupMultiReplies(sBuildZrange(zr, 1));

  RateLimitStatus st;
  enum RateLimitResultCode rc = RateLimitCheck(&sH, "testns", &sTestSpec, &st);

  CHECK(rc == RL_OK);
  CHECK_EQ(st.remaining_actions, -1);   /* exceeded by count */
  /* v1 = (4500000-5000000)+1000000 = 500000 us -> 500 ms until the window slides. */
  CHECK_EQ(st.remaining_time_ms, 500);
}

static void
tNotExceededNonEmpty(void)
{
  sReset();
  sInitScaffold();
  sTestSpec = sSpec(1000, 20, 0);
  sNowMicros = 5000000LL;

  const char *zr[] = {"4000000", "4500000", "4900000"};  /* 3 prior, well below max */
  sSetupMultiReplies(sBuildZrange(zr, 3));

  RateLimitStatus st;
  enum RateLimitResultCode rc = RateLimitCheck(&sH, "testns", &sTestSpec, &st);

  CHECK(rc == RL_OK);
  CHECK_EQ(st.remaining_time_ms, 0);
  /* Correct: 3 prior + this request = 4 used, 20 - 4 = 16 remain. */
  CHECK_EQ(st.remaining_actions, 16);
}

static void
tRequestsExceededBlocks(void)
{
  sReset();
  sInitScaffold();
  sNowMicros = 5000000LL;

  /* 20 prior requests == max_in_interval for RL_CAT_REQUESTS -> exceeded. */
  static const char *zr[20] = {
    "4000000","4050000","4100000","4150000","4200000","4250000","4300000",
    "4350000","4400000","4450000","4500000","4550000","4600000","4650000",
    "4700000","4750000","4800000","4850000","4900000","4950000",
  };
  sSetupMultiReplies(sBuildZrange(zr, 20));

  bool exceeded = RateLimitIsExceededForCategory(&sH, "testns", RL_CAT_REQUESTS);
  CHECK(exceeded == true);
}

static void
tSpecsValid(void)
{
  const RateLimitPolicy *c = RateLimitSpecsFor(RL_CAT_CONNECTONS);
  CHECK(c != NULL);
  CHECK_EQ(c->interval, 1000);
  CHECK_EQ(c->max_in_interval, 1);
  CHECK_EQ(c->min_difference, 100);

  const RateLimitPolicy *r = RateLimitSpecsFor(RL_CAT_REQUESTS);
  CHECK(r != NULL);
  CHECK_EQ(r->interval, 1000);
  CHECK_EQ(r->max_in_interval, 20);
  CHECK_EQ(r->min_difference, 0);
}

static void
tSpecsInvalid(void)
{
  CHECK(RateLimitSpecsFor(RL_CAT_MAXVALUE) == NULL);
  CHECK(RateLimitSpecsFor((enum RateLimitCategory)0) == NULL);
  CHECK(RateLimitSpecsFor((enum RateLimitCategory)-1) == NULL);
}

static void
tSessionNamespace(void)
{
  char buf[64];
  CHECK(RateLimitFormatSessionNamespace(buf, sizeof(buf), 42, 7, RL_CAT_CONNECTONS));
  CHECK(strcmp(buf, "42:7:1") == 0);
  CHECK(!RateLimitFormatSessionNamespace(buf, 4, 42, 7, RL_CAT_CONNECTONS));
}

static void
tErrorReturnsBackendError(void)
{
  sReset();
  sInitScaffold();
  sTestSpec = sSpec(1000, 20, 0);
  sNowMicros = 5000000LL;

  const char *zr[] = {};
  sSetupMultiReplies(sBuildZrange(zr, 0));
  sReplyErrAt = 3;                   /* force the ZADD reply to fail */

  RateLimitStatus st;
  enum RateLimitResultCode rc = RateLimitCheck(&sH, "testns", &sTestSpec, &st);

  CHECK(rc == RL_ERR_BACKEND);
  CHECK_EQ(sResult.code, RL_ERR_BACKEND);
}

static void
tWrongTypeSurfacesError(void)
{
  sReset();
  sInitScaffold();
  sTestSpec = sSpec(1000, 20, 0);
  sNowMicros = 5000000LL;

  /* Build the MULTI/EXEC sequence manually with an ERROR reply as the ZRANGE. */
  sReplies[0] = sNewStatus("OK");
  sReplies[1] = sNewStatus("QUEUED");
  sReplies[2] = sNewStatus("QUEUED");
  sReplies[3] = sNewStatus("QUEUED");
  sReplies[4] = sNewStatus("QUEUED");
  redisReply *exec = sNewArray(4);
  exec->element[0] = sNewInteger(0);
  exec->element[1] = sNewError("WRONGTYPE Operation against a key holding the wrong kind of value");
  exec->element[2] = sNewInteger(1);
  exec->element[3] = sNewInteger(1);
  sReplies[5] = exec;

  RateLimitStatus st;
  enum RateLimitResultCode rc = RateLimitCheck(&sH, "testns", &sTestSpec, &st);

  CHECK(rc == RL_ERR_BACKEND);
}

static void
tSubSecondIntervalExpiresOneSecond(void)
{
  sReset();
  sInitScaffold();
  sTestSpec = sSpec(500, 20, 0);           /* interval 500 ms */
  sNowMicros = 5000000LL;

  const char *zr[] = {};
  sSetupMultiReplies(sBuildZrange(zr, 0));

  RateLimitStatus st;
  enum RateLimitResultCode rc = RateLimitCheck(&sH, "testns", &sTestSpec, &st);

  CHECK(rc == RL_OK);
  CHECK(strstr(sCmds[4], "EXPIRE _RL:testns 1") != NULL);
}

static void
tNullGuards(void)
{
  sReset();
  sInitScaffold();
  sTestSpec = sSpec(1000, 20, 0);

  RateLimitStatus st;
  CHECK(RateLimitCheck(NULL, "testns", &sTestSpec, &st) == RL_ERR_MISSING_PARAM);
  CHECK(RateLimitCheck(&sH, NULL, &sTestSpec, &st) == RL_ERR_MISSING_PARAM);
  CHECK(RateLimitCheck(&sH, "testns", NULL, &st) == RL_ERR_MISSING_PARAM);
  CHECK(RateLimitCheck(&sH, "testns", &sTestSpec, NULL) == RL_ERR_MISSING_PARAM);
  CHECK(RateLimitIsExceeded(NULL, "testns", &sTestSpec) == true);   /* fail-closed */
}

static void
tInvalidNamespace(void)
{
  sReset();
  sInitScaffold();
  sTestSpec = sSpec(1000, 20, 0);

  RateLimitStatus st;
  CHECK(RateLimitCheck(&sH, "bad ns", &sTestSpec, &st) == RL_ERR_INVALID_NAMESPACE);
  CHECK(RateLimitCheck(&sH, "x%y", &sTestSpec, &st) == RL_ERR_INVALID_NAMESPACE);
  CHECK(RateLimitIsExceeded(&sH, "bad ns", &sTestSpec) == true);   /* fail-closed */
}

/* ── Runner ──────────────────────────────────────────────────────────────── */

typedef void (*sTest)(void);
typedef struct { const char *name; sTest fn; } sCase;

int
main(void)
{
  sHooks.pers = NULL;
  sHooks.ctx  = NULL;
  sHooks.send_command_multi = sFakeSendMulti;
  sHooks.get_reply          = sFakeGetReply;
  sHooks.free_reply         = freeReplyObject;
  sBe = RateLimitBackendRedisCreate(&sHooks);
  if (!sBe) {
    fprintf(stderr, "failed to create redis backend\n");
    return 1;
  }
  sH.backend = sBe;
  sH.result  = &sResult;

  const sCase tests[] = {
    {"command sequence + arithmetic",        tCommandSequenceAndArithmetic},
    {"empty set -> max-1",                  tEmptySet},
    {"exceeded by count",                   tExceededByCount},
    {"exceeded by min_difference",          tExceededByMinDifference},
    {"too many but min-diff satisfied",     tTooManyButMinDiffSatisfied},
    {"not exceeded (non-empty)",            tNotExceededNonEmpty},
    {"requests exceeded blocks",            tRequestsExceededBlocks},
    {"specs valid",                         tSpecsValid},
    {"specs invalid",                       tSpecsInvalid},
    {"session namespace",                   tSessionNamespace},
    {"error returns backend error",         tErrorReturnsBackendError},
    {"wrongtype surfaces error",            tWrongTypeSurfacesError},
    {"sub-second interval EXPIRE 1",        tSubSecondIntervalExpiresOneSecond},
    {"NULL guards",                         tNullGuards},
    {"invalid namespace",                   tInvalidNamespace},
  };

  printf("ratelimit_v2_unit_driver: tests\n");
  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    int before = sFailures;
    tests[i].fn();
    printf("  [%s] %s\n", sFailures == before ? "PASS" : "FAIL", tests[i].name);
  }

  printf("ratelimit_v2_unit_driver: %d checks, %d failures\n", sChecks, sFailures);
  RateLimitBackendDestroy(sBe);
  return sFailures == 0 ? 0 : 1;
}
