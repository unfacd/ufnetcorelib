/**
 * rl_sim — producer / consumer stress harness for the rate-limit gate.
 *
 * Models a resource-consuming layer (threads) trying to hog capacity while
 * RateLimitCheck() sits in the middle as the gate.  Memory backend only for now;
 * --backend redis is reserved for a later hook-up.
 *
 * Scenarios:
 *   1  single-key hog          — N hog threads, one key; aggregate ≤ max
 *   2  hog vs normal same key  — mixed roles; aggregate ≤ max
 *   3  hog isolated by key     — hog on key A must not starve key B
 *   4  min-difference          — allows spaced by min_difference
 *   5  window recovery         — after interval idle, allows resume
 *
 * Build (full include/flag set in tests/ratelimit/run_ratelimit_v2_test.sh):
 *   clang -std=gnu17 -D_GNU_SOURCE -fblocks -include stdatomic.h \
 *     -include config_defs_ufnetcorelib.h \
 *     -Iinclude/ufnetcorelib -Iinclude -Isrc/ufnet_core/ratelimit_v2 \
 *     src/ufnet_core/ratelimit_v2/ratelimit.c \
 *     src/ufnet_core/ratelimit_v2/ratelimit_backend_memory.c \
 *     ratelimitv2_simulator.c -o ratelimitv2_simulator -lm -lpthread
 *
 * Run:
 *   ./rl_sim                  # all scenarios
 *   ./rl_sim --scenario 3
 *   ./rl_sim --list
 */

#define _POSIX_C_SOURCE 200809L

#include <ufsrv_core/ratelimit_v2/ratelimit.h>
#include <ufnet_core/cache_backend/redis.h>
#include <ufnet_core/cache_backend/persistence.h>
#include <ufnet_core/thread_context_type.h>

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── Redis backend glue (only exercised with --backend redis) ────────────── */

/* The thread-local the library extern-declares; the app server normally owns it. */
__thread ThreadContext ufsrv_thread_context;

/* Wire a PersistenceBackend into a V2 redis backend through the hooks. */
static struct RateLimitBackend *
sBuildRedisBackend(PersistenceBackend *per)
{
	RateLimitRedisHooks hooks = {
		.pers               = per,
		.ctx                = NULL,
		.send_command_multi = RedisSendCommandMulti,
		.get_reply          = RedisGetReply,
		.free_reply         = freeReplyObject,
	};
	return RateLimitBackendRedisCreate(&hooks);
}

typedef enum { SIM_BACKEND_MEMORY, SIM_BACKEND_REDIS } SimBackendKind;

typedef struct {
	SimBackendKind            kind;
	struct RateLimitBackend  *mem;   /* shared memory backend (kind == MEMORY) */
	const char               *host;  /* redis host (kind == REDIS) */
	unsigned                  port;  /* redis port */
	const char               *label; /* redis descriptive label */
} BackendProvider;

/* Acquire a backend for the current thread.  Memory is the shared backend
 * (not owned); redis creates a fresh connection (owned; caller destroys). */
static struct RateLimitBackend *
sAcquireBackend(const BackendProvider *p, int *owned)
{
	if (p->kind == SIM_BACKEND_MEMORY) {
		*owned = 0;
		return p->mem;
	}
	*owned = 1;
	PersistenceBackend *per = InitialisePersistenceBackend(NULL, p->host, p->port, p->label, false);
	if (per == NULL)
		return NULL;
	return sBuildRedisBackend(per);
}

/* ── timing ─────────────────────────────────────────────────────────────── */

static long long
now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000LL + (long long)ts.tv_nsec / 1000LL;
}

static void
sleep_us(long long us)
{
	if (us <= 0)
		return;
	struct timespec ts = {
		.tv_sec  = (time_t)(us / 1000000LL),
		.tv_nsec = (long)((us % 1000000LL) * 1000LL),
	};
	nanosleep(&ts, NULL);
}

static void
sleep_ms(unsigned ms)
{
	sleep_us((long long)ms * 1000LL);
}

/* ── shared gate + counters ─────────────────────────────────────────────── */

typedef struct {
	const BackendProvider   *provider;
	RateLimitPolicy         spec;
	const char              *key;
	bool                     is_hog;       /* 0 think time vs polite sleep */
	unsigned                 think_ms;     /* normal clients only */
	long long                stop_at_us;   /* wall-clock end */
	/* per-thread stats (written only by owner) */
	unsigned long            allowed;
	unsigned long            denied;
	unsigned long            errors;
} ClientCtx;

typedef struct {
	unsigned long allowed;
	unsigned long denied;
	unsigned long errors;
} KeyTotals;

static bool
try_acquire(RateLimitHandle *h, const char *key, const RateLimitPolicy *spec,
            RateLimitStatus *st_out)
{
	RateLimitStatus st;
	enum RateLimitResultCode rc = RateLimitCheck(h, key, spec, &st);
	if (st_out)
		*st_out = st;
	if (rc != RL_OK)
		return false;
	return st.remaining_actions >= 0;
}

static void *
client_main(void *arg)
{
	ClientCtx *c = (ClientCtx *)arg;
	int owned = 0;
	struct RateLimitBackend *be = sAcquireBackend(c->provider, &owned);
	if (be == NULL) {
		c->errors++;
		return NULL;
	}
	RateLimitHandle h = { .backend = be, .result = NULL };

	while (now_us() < c->stop_at_us) {
		RateLimitStatus st;
		if (try_acquire(&h, c->key, &c->spec, &st)) {
			c->allowed++;
			if (!c->is_hog && c->think_ms > 0)
				sleep_ms(c->think_ms);
		} else {
			/* distinguish backend error vs limit via remaining_actions */
			if (st.remaining_actions < 0)
				c->denied++;
			else
				c->errors++;

			if (c->is_hog) {
				/* tight loop; tiny yield to avoid pure livelock on 1 core */
				sleep_us(100);
			} else {
				unsigned wait = c->think_ms;
				if (st.remaining_time_ms > 0 &&
				    st.remaining_time_ms < 5000)
					wait = (unsigned)st.remaining_time_ms;
				if (wait == 0)
					wait = 1;
				sleep_ms(wait);
			}
		}
	}

	if (owned)
		RateLimitBackendDestroy(be);
	return NULL;
}

/* ── reporting helpers ──────────────────────────────────────────────────── */

static void
sum_clients(ClientCtx *cs, int n, KeyTotals *out)
{
	memset(out, 0, sizeof(*out));
	for (int i = 0; i < n; i++) {
		out->allowed += cs[i].allowed;
		out->denied  += cs[i].denied;
		out->errors  += cs[i].errors;
	}
}

static void
print_clients(const char *label, ClientCtx *cs, int n)
{
	printf("  [%s]\n", label);
	for (int i = 0; i < n; i++) {
		printf("    client %2d key=%-12s hog=%d  allow=%lu deny=%lu err=%lu\n",
		       i, cs[i].key, (int)cs[i].is_hog,
		       cs[i].allowed, cs[i].denied, cs[i].errors);
	}
}

/* ── scenario runners ───────────────────────────────────────────────────── */

/*
 * Scenario 1 — single-key hog
 * N hog threads share one key.  Over a run of ~1.5 intervals, total allows
 * must not exceed max_in_interval * ceil(duration/interval) by more than a
 * small slack (we assert a hard upper bound of max * windows + max).
 */
static int
scenario_1_single_key_hog(const BackendProvider *p)
{
	printf("\n=== scenario 1: single-key hog ===\n");

	const int n_hogs = 16;
	const unsigned duration_ms = 1500;
	RateLimitPolicy spec = {
		.interval = 1000, .max_in_interval = 10, .min_difference = 0
	};
	const char *key = "sim:s1:user";

	int owned = 0;
	struct RateLimitBackend *be = sAcquireBackend(p, &owned);
	if (be == NULL) {
		fprintf(stderr, "  FAIL: backend acquire failed\n");
		return 1;
	}

	RateLimitBackendClear(be, key);

	ClientCtx *cs = calloc((size_t)n_hogs, sizeof(ClientCtx));
	pthread_t *tids = calloc((size_t)n_hogs, sizeof(pthread_t));
	long long stop = now_us() + (long long)duration_ms * 1000LL;

	for (int i = 0; i < n_hogs; i++) {
		cs[i].provider  = p;
		cs[i].spec      = spec;
		cs[i].key       = key;
		cs[i].is_hog    = true;
		cs[i].think_ms  = 0;
		cs[i].stop_at_us = stop;
		pthread_create(&tids[i], NULL, client_main, &cs[i]);
	}
	for (int i = 0; i < n_hogs; i++)
		pthread_join(tids[i], NULL);

	KeyTotals tot;
	sum_clients(cs, n_hogs, &tot);
	print_clients("s1", cs, n_hogs);

	/* duration covers at most 2 full windows → hard cap 2 * max + max slack */
	unsigned long hard_cap =
	    (unsigned long)spec.max_in_interval * 3UL; /* generous but finite */

	printf("  total allow=%lu deny=%lu  hard_cap=%lu (max=%zu)\n",
	       tot.allowed, tot.denied, hard_cap, spec.max_in_interval);

	int fail = 0;
	if (tot.allowed == 0) {
		fprintf(stderr, "  FAIL: expected some allows\n");
		fail = 1;
	}
	if (tot.allowed > hard_cap) {
		fprintf(stderr, "  FAIL: allows %lu exceeded hard_cap %lu\n",
		        tot.allowed, hard_cap);
		fail = 1;
	}
	if (tot.denied == 0) {
		fprintf(stderr, "  FAIL: expected denials under hog pressure\n");
		fail = 1;
	}
	if (tot.errors != 0) {
		fprintf(stderr, "  FAIL: unexpected backend errors %lu\n", tot.errors);
		fail = 1;
	}

	/* Tighter: sample a fresh window alone — sequential burst must stop at max */
	RateLimitBackendClear(be, key);
	RateLimitHandle h = { .backend = be, .result = NULL };
	unsigned seq_ok = 0;
	for (unsigned i = 0; i < spec.max_in_interval + 5; i++) {
		if (try_acquire(&h, key, &spec, NULL))
			seq_ok++;
	}
	printf("  sequential burst allows=%u (want == %zu)\n",
	       seq_ok, spec.max_in_interval);
	if (seq_ok != (unsigned)spec.max_in_interval) {
		fprintf(stderr, "  FAIL: sequential burst allowed %u, want %zu\n",
		        seq_ok, spec.max_in_interval);
		fail = 1;
	}

	free(cs);
	free(tids);
	if (owned)
		RateLimitBackendDestroy(be);
	printf("  %s\n", fail ? "FAIL" : "PASS");
	return fail;
}

/*
 * Scenario 2 — hog vs normal on the same key
 * Aggregate still ≤ hard_cap; both roles should see some activity.
 */
static int
scenario_2_hog_vs_normal(const BackendProvider *p)
{
	printf("\n=== scenario 2: hog vs normal (same key) ===\n");

	const int n_normal = 4;
	const int n_hog = 12;
	const int n = n_normal + n_hog;
	const unsigned duration_ms = 1500;
	RateLimitPolicy spec = {
		.interval = 1000, .max_in_interval = 15, .min_difference = 0
	};
	const char *key = "sim:s2:shared";

	int owned = 0;
	struct RateLimitBackend *be = sAcquireBackend(p, &owned);
	if (be == NULL) {
		fprintf(stderr, "  FAIL: backend acquire failed\n");
		return 1;
	}

	RateLimitBackendClear(be, key);

	ClientCtx *cs = calloc((size_t)n, sizeof(ClientCtx));
	pthread_t *tids = calloc((size_t)n, sizeof(pthread_t));
	long long stop = now_us() + (long long)duration_ms * 1000LL;

	for (int i = 0; i < n; i++) {
		cs[i].provider   = p;
		cs[i].spec       = spec;
		cs[i].key        = key;
		cs[i].is_hog     = (i >= n_normal);
		cs[i].think_ms   = cs[i].is_hog ? 0 : 40;
		cs[i].stop_at_us = stop;
		pthread_create(&tids[i], NULL, client_main, &cs[i]);
	}
	for (int i = 0; i < n; i++)
		pthread_join(tids[i], NULL);

	KeyTotals tot, hog_tot, norm_tot;
	sum_clients(cs, n, &tot);
	sum_clients(cs, n_normal, &norm_tot);
	sum_clients(cs + n_normal, n_hog, &hog_tot);
	print_clients("s2", cs, n);

	unsigned long hard_cap = (unsigned long)spec.max_in_interval * 3UL;
	printf("  total allow=%lu (normal=%lu hog=%lu) deny=%lu  hard_cap=%lu\n",
	       tot.allowed, norm_tot.allowed, hog_tot.allowed, tot.denied, hard_cap);

	int fail = 0;
	if (tot.allowed > hard_cap) {
		fprintf(stderr, "  FAIL: aggregate allows %lu > hard_cap %lu\n",
		        tot.allowed, hard_cap);
		fail = 1;
	}
	if (tot.denied == 0) {
		fprintf(stderr, "  FAIL: expected denials\n");
		fail = 1;
	}
	if (hog_tot.allowed == 0 && norm_tot.allowed == 0) {
		fprintf(stderr, "  FAIL: no allows at all\n");
		fail = 1;
	}
	/* Note: pure rate-limit is not fair — hogs may dominate; that is OK. */

	free(cs);
	free(tids);
	if (owned)
		RateLimitBackendDestroy(be);
	printf("  %s\n", fail ? "FAIL" : "PASS");
	return fail;
}

/*
 * Scenario 3 — hog isolated by key
 * Hogs hammer key A; normals use key B.  Key B must still get its budget.
 */
static int
scenario_3_key_isolation(const BackendProvider *p)
{
	printf("\n=== scenario 3: hog isolated by key ===\n");

	const int n_hog = 12;
	const int n_normal = 4;
	const int n = n_hog + n_normal;
	const unsigned duration_ms = 1200;
	RateLimitPolicy spec = {
		.interval = 1000, .max_in_interval = 10, .min_difference = 0
	};
	const char *key_hog = "sim:s3:hog";
	const char *key_ok  = "sim:s3:victim";

	int owned = 0;
	struct RateLimitBackend *be = sAcquireBackend(p, &owned);
	if (be == NULL) {
		fprintf(stderr, "  FAIL: backend acquire failed\n");
		return 1;
	}

	RateLimitBackendClear(be, key_hog);
	RateLimitBackendClear(be, key_ok);

	ClientCtx *cs = calloc((size_t)n, sizeof(ClientCtx));
	pthread_t *tids = calloc((size_t)n, sizeof(pthread_t));
	long long stop = now_us() + (long long)duration_ms * 1000LL;

	for (int i = 0; i < n_hog; i++) {
		cs[i].provider   = p;
		cs[i].spec       = spec;
		cs[i].key        = key_hog;
		cs[i].is_hog     = true;
		cs[i].think_ms   = 0;
		cs[i].stop_at_us = stop;
		pthread_create(&tids[i], NULL, client_main, &cs[i]);
	}
	for (int i = 0; i < n_normal; i++) {
		int idx = n_hog + i;
		cs[idx].provider   = p;
		cs[idx].spec       = spec;
		cs[idx].key        = key_ok;
		cs[idx].is_hog     = false;
		cs[idx].think_ms   = 20;
		cs[idx].stop_at_us = stop;
		pthread_create(&tids[idx], NULL, client_main, &cs[idx]);
	}
	for (int i = 0; i < n; i++)
		pthread_join(tids[i], NULL);

	KeyTotals hog_tot, ok_tot;
	sum_clients(cs, n_hog, &hog_tot);
	sum_clients(cs + n_hog, n_normal, &ok_tot);
	print_clients("s3", cs, n);

	printf("  hog key allow=%lu deny=%lu | victim key allow=%lu deny=%lu\n",
	       hog_tot.allowed, hog_tot.denied, ok_tot.allowed, ok_tot.denied);

	int fail = 0;
	unsigned long hard_cap = (unsigned long)spec.max_in_interval * 3UL;
	if (hog_tot.allowed > hard_cap) {
		fprintf(stderr, "  FAIL: hog key allows %lu > hard_cap %lu\n",
		        hog_tot.allowed, hard_cap);
		fail = 1;
	}
	if (ok_tot.allowed > hard_cap) {
		fprintf(stderr, "  FAIL: victim key allows %lu > hard_cap %lu\n",
		        ok_tot.allowed, hard_cap);
		fail = 1;
	}
	/* Victim must not be starved: expect at least max_in_interval allows
	 * over ~1.2s with polite clients (they fit easily within budget). */
	if (ok_tot.allowed < (unsigned long)spec.max_in_interval) {
		fprintf(stderr,
		        "  FAIL: victim key starved (allow=%lu, want >= %zu)\n",
		        ok_tot.allowed, spec.max_in_interval);
		fail = 1;
	}
	if (hog_tot.denied == 0) {
		fprintf(stderr, "  FAIL: hog key expected denials\n");
		fail = 1;
	}

	free(cs);
	free(tids);
	if (owned)
		RateLimitBackendDestroy(be);
	printf("  %s\n", fail ? "FAIL" : "PASS");
	return fail;
}

/*
 * Scenario 4 — min-difference
 *
 * Note: every RateLimitCheck records an event (including denials), so a pure spin
 * keeps refreshing "newest" and the gap never opens.  That is intentional
 * gate semantics (matches the Redis ZADD-always path).
 *
 * We therefore assert:
 *   (a) rapid-fire after the first allow → sustained denials
 *   (b) sleep ≥ min_difference with no intervening checks → next allow works
 *   (c) polite client that waits remaining_time_ms gets spaced allows
 */
static int
scenario_4_min_difference(const BackendProvider *p)
{
	printf("\n=== scenario 4: min-difference ===\n");

	/* max_in_interval kept high so only min_difference gates this scenario */
	RateLimitPolicy spec = {
		.interval = 5000, .max_in_interval = 1000, .min_difference = 100
	};
	const char *key = "sim:s4:gap";

	int owned = 0;
	struct RateLimitBackend *be = sAcquireBackend(p, &owned);
	if (be == NULL) {
		fprintf(stderr, "  FAIL: backend acquire failed\n");
		return 1;
	}

	RateLimitBackendClear(be, key);

	RateLimitHandle h = { .backend = be, .result = NULL };
	int fail = 0;

	/* (a) first allow, then rapid-fire denials (spin refreshes newest) */
	RateLimitStatus st;
	if (!try_acquire(&h, key, &spec, &st)) {
		fprintf(stderr, "  FAIL: first acquire should allow\n");
		fail = 1;
	} else {
		printf("  first acquire: ALLOWED\n");
	}

	int rapid_deny = 0;
	int rapid_allow = 0;
	for (int i = 0; i < 50; i++) {
		if (try_acquire(&h, key, &spec, &st))
			rapid_allow++;
		else
			rapid_deny++;
	}
	printf("  rapid-fire after first: allow=%d deny=%d\n",
	       rapid_allow, rapid_deny);
	if (rapid_allow != 0) {
		fprintf(stderr,
		        "  FAIL: rapid-fire should not allow while spinning "
		        "(newest keeps refreshing)\n");
		fail = 1;
	}
	if (rapid_deny < 50) {
		fprintf(stderr, "  FAIL: expected all rapid attempts denied\n");
		fail = 1;
	}

	/* (b) idle ≥ min_difference with *no* intervening checks → allow */
	printf("  sleeping %zu ms with no checks...\n", spec.min_difference + 20);
	sleep_ms((unsigned)spec.min_difference + 20);

	if (!try_acquire(&h, key, &spec, &st)) {
		fprintf(stderr,
		        "  FAIL: expected allow after idle ≥ min_difference "
		        "(remaining_time_ms was %zu ms)\n",
		        st.remaining_time_ms);
		fail = 1;
	} else {
		printf("  after idle: ALLOWED\n");
	}

	/* (c) polite client: on deny, sleep remaining_time_ms; collect gaps */
	RateLimitBackendClear(be, key);
	long long allow_times[6];
	int n_allow = 0;
	long long deadline = now_us() + 800000LL; /* 800 ms */

	while (now_us() < deadline && n_allow < 6) {
		if (try_acquire(&h, key, &spec, &st)) {
			allow_times[n_allow++] = now_us();
		} else {
			unsigned wait = (unsigned)st.remaining_time_ms;
			if (wait == 0)
				wait = (unsigned)spec.min_difference;
			/* cap so we do not overshoot the deadline badly */
			if (wait > 200)
				wait = 200;
			sleep_ms(wait);
		}
	}

	printf("  polite allows=%d\n", n_allow);
	if (n_allow < 3) {
		fprintf(stderr, "  FAIL: polite client expected ≥3 allows, got %d\n",
		        n_allow);
		fail = 1;
	}

	int gaps_bad = 0;
	for (int i = 1; i < n_allow; i++) {
		long long gap_ms = (allow_times[i] - allow_times[i - 1]) / 1000LL;
		printf("  gap[%d]=%lld ms\n", i, gap_ms);
		/* 70% slack: sleep scheduling + check overhead */
		if (gap_ms < (long long)spec.min_difference * 70 / 100)
			gaps_bad++;
	}
	if (gaps_bad > 0) {
		fprintf(stderr, "  FAIL: %d gaps shorter than ~70%% of min_difference\n",
		        gaps_bad);
		fail = 1;
	}

	if (owned)
		RateLimitBackendDestroy(be);
	printf("  %s\n", fail ? "FAIL" : "PASS");
	return fail;
}

/*
 * Scenario 5 — window recovery
 * Exhaust the window, sleep interval, confirm allows resume without clear().
 */
static int
scenario_5_window_recovery(const BackendProvider *p)
{
	printf("\n=== scenario 5: window recovery ===\n");

	RateLimitPolicy spec = {
		.interval = 300, .max_in_interval = 5, .min_difference = 0
	};
	const char *key = "sim:s5:recover";

	int owned = 0;
	struct RateLimitBackend *be = sAcquireBackend(p, &owned);
	if (be == NULL) {
		fprintf(stderr, "  FAIL: backend acquire failed\n");
		return 1;
	}

	RateLimitBackendClear(be, key);

	RateLimitHandle h = { .backend = be, .result = NULL };

	unsigned first_wave = 0;
	for (unsigned i = 0; i < spec.max_in_interval + 10; i++) {
		if (try_acquire(&h, key, &spec, NULL))
			first_wave++;
	}
	printf("  first wave allows=%u (want %zu)\n",
	       first_wave, spec.max_in_interval);

	int fail = 0;
	if (first_wave != (unsigned)spec.max_in_interval) {
		fprintf(stderr, "  FAIL: first wave %u != max %zu\n",
		        first_wave, spec.max_in_interval);
		fail = 1;
	}

	/* Still denied immediately */
	if (try_acquire(&h, key, &spec, NULL)) {
		fprintf(stderr, "  FAIL: expected deny before window slide\n");
		fail = 1;
	} else {
		printf("  pre-sleep: DENIED (good)\n");
	}

	printf("  sleeping %zu ms for window to slide...\n", spec.interval + 50);
	sleep_ms((unsigned)spec.interval + 50);

	unsigned second_wave = 0;
	for (unsigned i = 0; i < spec.max_in_interval; i++) {
		if (try_acquire(&h, key, &spec, NULL))
			second_wave++;
	}
	printf("  second wave allows=%u (want %zu)\n",
	       second_wave, spec.max_in_interval);

	if (second_wave != (unsigned)spec.max_in_interval) {
		fprintf(stderr, "  FAIL: recovery allows %u != max %zu\n",
		        second_wave, spec.max_in_interval);
		fail = 1;
	}

	if (owned)
		RateLimitBackendDestroy(be);
	printf("  %s\n", fail ? "FAIL" : "PASS");
	return fail;
}

/* ── CLI ────────────────────────────────────────────────────────────────── */

static void
usage(const char *prog)
{
	fprintf(stderr,
	        "usage: %s [--scenario N|all] [--backend memory|redis] [--host H] [--port P]\n"
	        "       %s --list\n"
	        "\n"
	        "scenarios:\n"
	        "  1  single-key hog\n"
	        "  2  hog vs normal (same key)\n"
	        "  3  hog isolated by key\n"
	        "  4  min-difference spacing\n"
	        "  5  window recovery after idle\n",
	        prog, prog);
}

int
main(int argc, char **argv)
{
	int scenario = 0; /* 0 = all */
	const char *backend_name = "memory";
	const char *host = "127.0.0.1";
	unsigned port = 63793; /* local redis-usrmsg */
	const char *label = "rl-sim";

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--list") == 0) {
			printf("1 single-key hog\n"
			       "2 hog vs normal (same key)\n"
			       "3 hog isolated by key\n"
			       "4 min-difference\n"
			       "5 window recovery\n");
			return 0;
		}
		if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
			const char *v = argv[++i];
			if (strcmp(v, "all") == 0)
				scenario = 0;
			else
				scenario = (int)strtol(v, NULL, 10);
		} else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
			backend_name = argv[++i];
		} else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
			host = argv[++i];
		} else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
			port = (unsigned)strtoul(argv[++i], NULL, 10);
		} else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
			label = argv[++i];
		} else if (strcmp(argv[i], "--help") == 0 ||
		           strcmp(argv[i], "-h") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	BackendProvider p = {0};
	if (strcmp(backend_name, "memory") == 0) {
		p.kind = SIM_BACKEND_MEMORY;
		p.mem  = RateLimitBackendMemoryCreate();
		if (p.mem == NULL) {
			fprintf(stderr, "RateLimitBackendMemoryCreate failed\n");
			return 1;
		}
	} else if (strcmp(backend_name, "redis") == 0) {
		p.kind  = SIM_BACKEND_REDIS;
		p.host  = host;
		p.port  = port;
		p.label = label;
	} else {
		fprintf(stderr, "backend must be memory or redis (got '%s')\n", backend_name);
		return 2;
	}

	printf("ratelimitv2_simulator backend=%s", backend_name);
	if (p.kind == SIM_BACKEND_REDIS)
		printf(" (%s:%u)", p.host, p.port);
	printf("\n");

	int failures = 0;
	typedef int (*scenario_fn)(const BackendProvider *);
	scenario_fn all[] = {
		scenario_1_single_key_hog,
		scenario_2_hog_vs_normal,
		scenario_3_key_isolation,
		scenario_4_min_difference,
		scenario_5_window_recovery,
	};

	if (scenario == 0) {
		for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
			failures += all[i](&p);
	} else if (scenario >= 1 && scenario <= 5) {
		failures += all[scenario - 1](&p);
	} else {
		fprintf(stderr, "unknown scenario %d\n", scenario);
		if (p.kind == SIM_BACKEND_MEMORY)
			RateLimitBackendDestroy(p.mem);
		return 2;
	}

	if (p.kind == SIM_BACKEND_MEMORY)
		RateLimitBackendDestroy(p.mem);

	printf("\nratelimitv2_simulator: %s (%d scenario failure(s))\n",
	       failures == 0 ? "PASS" : "FAIL", failures);
	return failures == 0 ? 0 : 1;
}
