/**
 * @file ratelimit_v2_test_memory.c
 * @brief Deterministic tests for the ratelimit_v2 memory backend + core.
 *
 * No Redis required.  Exercises the core sliding-window arithmetic and the
 * in-process backend through the public API.
 *
 * Build (see run_ratelimit_v2_test.sh):
 *   cc -std=gnu17 ratelimit.c ratelimit_backend_memory.c \
 *      ratelimit_v2_test_memory.c -lpthread -o ratelimit_v2_test_memory
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

#include <stdio.h>
#include <string.h>

#include <ufsrv_core/ratelimit_v2/ratelimit.h>

static int sChecks;
static int sFailures;

#define CHECK(cond)                                                            \
  do {                                                                        \
    sChecks++;                                                               \
    if (!(cond)) {                                                            \
      sFailures++;                                                           \
      fprintf(stderr, "  FAIL %s:%d: %s\n", __func__, __LINE__, #cond);      \
    }                                                                         \
  } while (0)

#define CHECK_EQ(a, b)                                                         \
  do {                                                                        \
    long long _a = (long long)(a);                                            \
    long long _b = (long long)(b);                                            \
    sChecks++;                                                               \
    if (_a != _b) {                                                           \
      sFailures++;                                                           \
      fprintf(stderr, "  FAIL %s:%d: %s == %s (got %lld, want %lld)\n",      \
              __func__, __LINE__, #a, #b, _a, _b);                            \
    }                                                                         \
  } while (0)

static void
tNamespaceValidation(void)
{
  CHECK(RateLimitIsNamespaceValid("user:1:2") == true);
  CHECK(RateLimitIsNamespaceValid("10.0.0.1") == true);
  CHECK(RateLimitIsNamespaceValid("") == false);
  CHECK(RateLimitIsNamespaceValid(NULL) == false);
  CHECK(RateLimitIsNamespaceValid("bad key") == false);
  CHECK(RateLimitIsNamespaceValid("has%fmt") == false);
  CHECK(RateLimitIsNamespaceValid("has\ttab") == false);
}

static void
tSpecs(void)
{
  const RateLimitPolicy *c = RateLimitSpecsFor(RL_CAT_CONNECTONS);
  CHECK(c != NULL);
  CHECK_EQ(c->max_in_interval, 1);
  CHECK_EQ(c->min_difference, 100);

  const RateLimitPolicy *r = RateLimitSpecsFor(RL_CAT_REQUESTS);
  CHECK(r != NULL);
  CHECK_EQ(r->max_in_interval, 20);

  CHECK(RateLimitSpecsFor(RL_CAT_MAXVALUE) == NULL);
  CHECK(RateLimitSpecsFor((enum RateLimitCategory)0) == NULL);
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
tEmptyWindow(void)
{
  struct RateLimitBackend *be = RateLimitBackendMemoryCreate();
  CHECK(be != NULL);
  RateLimitResult res = {0};
  RateLimitHandle h = { .backend = be, .result = &res };

  RateLimitPolicy spec = { .interval = 1000, .max_in_interval = 5, .min_difference = 0 };
  RateLimitStatus st;
  CHECK(RateLimitCheck(&h, "ns_empty", &spec, &st) == RL_OK);
  CHECK_EQ(st.remaining_actions, 4);
  CHECK_EQ(st.remaining_time_ms, 0);
  CHECK_EQ(res.code, RL_OK);

  RateLimitBackendDestroy(be);
}

static void
tCountLimit(void)
{
  struct RateLimitBackend *be = RateLimitBackendMemoryCreate();
  RateLimitHandle h = { .backend = be, .result = NULL };

  RateLimitPolicy spec = { .interval = 5000, .max_in_interval = 3, .min_difference = 0 };

  RateLimitStatus st;
  CHECK(RateLimitCheck(&h, "ns_count", &spec, &st) == RL_OK);
  CHECK_EQ(st.remaining_actions, 2); /* first of 3 */

  CHECK(RateLimitCheck(&h, "ns_count", &spec, &st) == RL_OK);
  CHECK_EQ(st.remaining_actions, 1);

  CHECK(RateLimitCheck(&h, "ns_count", &spec, &st) == RL_OK);
  CHECK_EQ(st.remaining_actions, 0); /* last allowed */

  CHECK(RateLimitCheck(&h, "ns_count", &spec, &st) == RL_OK);
  CHECK_EQ(st.remaining_actions, -1); /* exceeded */
  CHECK(st.remaining_time_ms > 0);

  CHECK(RateLimitIsExceeded(&h, "ns_count", &spec) == true);

  RateLimitBackendDestroy(be);
}

static void
tMinDifference(void)
{
  struct RateLimitBackend *be = RateLimitBackendMemoryCreate();
  RateLimitHandle h = { .backend = be, .result = NULL };

  RateLimitPolicy spec = { .interval = 5000, .max_in_interval = 20, .min_difference = 50 };

  RateLimitStatus st;
  CHECK(RateLimitCheck(&h, "ns_gap", &spec, &st) == RL_OK);
  CHECK(st.remaining_actions >= 0);

  /* Immediate second call should violate min_difference. */
  CHECK(RateLimitCheck(&h, "ns_gap", &spec, &st) == RL_OK);
  CHECK_EQ(st.remaining_actions, -1);
  CHECK(st.remaining_time_ms > 0);

  RateLimitBackendDestroy(be);
}

static void
tClearAndRecover(void)
{
  struct RateLimitBackend *be = RateLimitBackendMemoryCreate();
  RateLimitHandle h = { .backend = be, .result = NULL };

  RateLimitPolicy spec = { .interval = 5000, .max_in_interval = 1, .min_difference = 0 };

  RateLimitStatus st;
  CHECK(RateLimitCheck(&h, "ns_clear", &spec, &st) == RL_OK);
  CHECK_EQ(st.remaining_actions, 0);
  CHECK(RateLimitCheck(&h, "ns_clear", &spec, &st) == RL_OK);
  CHECK_EQ(st.remaining_actions, -1);

  CHECK(RateLimitBackendClear(be, "ns_clear") == RL_OK);
  CHECK(RateLimitCheck(&h, "ns_clear", &spec, &st) == RL_OK);
  CHECK_EQ(st.remaining_actions, 0);

  RateLimitBackendDestroy(be);
}

static void
tInvalidNamespaceRejected(void)
{
  struct RateLimitBackend *be = RateLimitBackendMemoryCreate();
  RateLimitHandle h = { .backend = be, .result = NULL };
  RateLimitPolicy spec = { .interval = 1000, .max_in_interval = 5 };
  RateLimitStatus st;

  CHECK(RateLimitCheck(&h, "bad ns", &spec, &st) == RL_ERR_INVALID_NAMESPACE);
  CHECK(RateLimitCheck(&h, "x%y", &spec, &st) == RL_ERR_INVALID_NAMESPACE);
  CHECK(RateLimitIsExceeded(&h, "bad ns", &spec) == true); /* fail-closed */

  RateLimitBackendDestroy(be);
}

static void
tCategoryHelper(void)
{
  struct RateLimitBackend *be = RateLimitBackendMemoryCreate();
  RateLimitHandle h = { .backend = be, .result = NULL };

  /* RL_CAT_CONNECTONS: max 1 */
  CHECK(RateLimitIsExceededForCategory(&h, "cat:1", RL_CAT_CONNECTONS) == false);
  CHECK(RateLimitIsExceededForCategory(&h, "cat:1", RL_CAT_CONNECTONS) == true);

  RateLimitBackendDestroy(be);
}

int
main(void)
{
  struct {
    const char *name;
    void (*fn)(void);
  } tests[] = {
    { "namespace validation", tNamespaceValidation },
    { "specs", tSpecs },
    { "session namespace", tSessionNamespace },
    { "empty window", tEmptyWindow },
    { "count limit", tCountLimit },
    { "min difference", tMinDifference },
    { "clear and recover", tClearAndRecover },
    { "invalid namespace", tInvalidNamespaceRejected },
    { "category helper", tCategoryHelper },
  };

  printf("ratelimit_v2_test_memory\n");
  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    int before = sFailures;
    tests[i].fn();
    printf("  [%s] %s\n", sFailures == before ? "PASS" : "FAIL", tests[i].name);
  }
  printf("ratelimit_v2_test_memory: %d checks, %d failures\n", sChecks, sFailures);
  return sFailures == 0 ? 0 : 1;
}
