#!/usr/bin/env bash
#
# One-invocation entry point for the ufnetcorelib ratelimit_v2 test suite.
#
# Builds three standalone drivers plus a multi-threaded stress simulator from
# the *production* translation units and runs them, deliberately NOT via the
# full ufnetcorelib cmake build (whose ufsrv_config composer emits stale stubs —
# see tests/redis/run_toxiproxy_redis_test.sh):
#
#   ratelimit_v2_unit_driver    — deterministic, no live Redis. Injects the
#                                 backend hooks and overrides the weak clock
#                                 symbol; asserts the command argv-structure,
#                                 status arithmetic, and the
#                                 fixed-defect regression behaviour.
#   ratelimit_v2_test_memory    — deterministic memory-backend tests.
#   ratelimit_v2_simulator      — producer/hog stress simulator (memory backend);
#                                 5 scenarios: hog throttle, mixed fairness, key
#                                 isolation, min-difference, window recovery.
#   ratelimit_v2_driver         — live-Redis integration. Links ratelimit.c +
#                                 ratelimit_backend_redis.c + redis.c +
#                                 persistence.c + vendored hiredis + uflib.
#
# Usage:
#   ./tests/ratelimit/run_ratelimit_v2_test.sh              # unit + memory + sim + integration
#   ./tests/ratelimit/run_ratelimit_v2_test.sh unit         # unit only
#   ./tests/ratelimit/run_ratelimit_v2_test.sh memory       # memory only
#   ./tests/ratelimit/run_ratelimit_v2_test.sh sim          # stress simulator (memory)
#   ./tests/ratelimit/run_ratelimit_v2_test.sh sim-redis    # stress simulator (local Redis)
#   ./tests/ratelimit/run_ratelimit_v2_test.sh single       # one integration scenario
#
# Exit 0 if every requested scenario passed, non-zero otherwise.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD_DIR="$ROOT/build/ratelimit-v2-test"

UFLIB_LIB="${UFLIB_LIB:-/usr/local/lib/libuflib.a}"
UFLIB_INC="${UFLIB_INC:-/usr/local/include/uflib}"
CC="${CC:-clang}"

SCENARIO="${1:-all}"
case "$SCENARIO" in
  all|unit|memory|sim|sim-redis|single|concurrency|collapse|connect_down) ;;
  *) echo "usage: $0 [all|unit|memory|sim|sim-redis|single|concurrency|collapse|connect_down]" >&2; exit 2 ;;
esac

# ── Shared include / flag sets (mirrors the redis harness) ─────────────────
inc_ufnet=(
  -I"$ROOT/include/ufnetcorelib"
  -I"$ROOT/packages"
  -I"$ROOT/packages/hiredis"
  -I"$ROOT/packages/ufsrvcorelib/include"
  -I"$ROOT/packages/ufsrvcorelib/include/ufsrvcorelib"
  -I"$ROOT/include"
  -I"$ROOT/src/ufnet_core/cache_backend"
  -isystem /opt/include
  -isystem /opt/include/mariadb
  -isystem "$UFLIB_INC"
)
cflags_common=(-std=gnu17 -D_GNU_SOURCE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
cflags_ufnet=("${cflags_common[@]}" -fblocks -include stdatomic.h -include config_defs_ufnetcorelib.h "${inc_ufnet[@]}")

# ── Vendored hiredis objects (built once) ──────────────────────────────────
build_hiredis() {
  mkdir -p "$BUILD_DIR"
  local hiredis_objs=()
  local src
  for src in async dict hiredis net sds sockcompat read; do
    if [[ ! -f "$BUILD_DIR/hiredis_$src.o" ]]; then
      "$CC" -std=gnu17 -D_GNU_SOURCE -I"$ROOT/packages/hiredis" \
        -c "$ROOT/packages/hiredis/$src.c" -o "$BUILD_DIR/hiredis_$src.o"
    fi
    hiredis_objs+=("$BUILD_DIR/hiredis_$src.o")
  done
  printf '%s\n' "${hiredis_objs[@]}"
}

# ── ratelimit_v2 objects ───────────────────────────────────────────────────
RL_C="$ROOT/src/ufnet_core/ratelimit_v2/ratelimit.c"
RL_MEM_C="$ROOT/src/ufnet_core/ratelimit_v2/ratelimit_backend_memory.c"
RL_REDIS_C="$ROOT/src/ufnet_core/ratelimit_v2/ratelimit_backend_redis.c"

build_ratelimit_obj() {
  mkdir -p "$BUILD_DIR"
  "$CC" "${cflags_ufnet[@]}" -c "$RL_C" -o "$BUILD_DIR/ratelimit.o"
}
build_ratelimit_memory_obj() {
  mkdir -p "$BUILD_DIR"
  "$CC" "${cflags_ufnet[@]}" -c "$RL_MEM_C" -o "$BUILD_DIR/ratelimit_backend_memory.o"
}
build_ratelimit_redis_obj() {
  mkdir -p "$BUILD_DIR"
  "$CC" "${cflags_ufnet[@]}" -c "$RL_REDIS_C" -o "$BUILD_DIR/ratelimit_backend_redis.o"
}

# ── Unit driver (no live Redis; injects hooks, overrides the clock) ─────────
build_unit() {
  echo "[run] building standalone unit driver"
  mapfile -t hiredis_objs < <(build_hiredis)
  build_ratelimit_obj
  build_ratelimit_redis_obj

  "$CC" "${cflags_ufnet[@]}" -c "$HERE/ratelimit_v2_unit_driver.c" -o "$BUILD_DIR/unit_driver.o"

  "$CC" "${cflags_common[@]}" -o "$BUILD_DIR/ratelimit_v2_unit_driver" \
    "$BUILD_DIR/unit_driver.o" "$BUILD_DIR/ratelimit.o" \
    "$BUILD_DIR/ratelimit_backend_redis.o" \
    "${hiredis_objs[@]}" "$UFLIB_LIB" -lm -lpthread
}

# ── Memory driver (no Redis, no hiredis) ───────────────────────────────────
build_memory() {
  echo "[run] building standalone memory driver"
  build_ratelimit_obj
  build_ratelimit_memory_obj

  "$CC" "${cflags_ufnet[@]}" -c "$HERE/ratelimit_v2_test_memory.c" -o "$BUILD_DIR/memory_driver.o"

  "$CC" "${cflags_common[@]}" -o "$BUILD_DIR/ratelimit_v2_test_memory" \
    "$BUILD_DIR/memory_driver.o" "$BUILD_DIR/ratelimit.o" \
    "$BUILD_DIR/ratelimit_backend_memory.o" \
    -lm -lpthread
}

# ── Stress simulator (memory + redis backends; producer/hog threads) ───────
build_sim() {
  echo "[run] building standalone stress simulator"
  mapfile -t hiredis_objs < <(build_hiredis)
  build_ratelimit_obj
  build_ratelimit_memory_obj
  build_ratelimit_redis_obj

  "$CC" "${cflags_ufnet[@]}" -c "$HERE/ratelimitv2_simulator.c" -o "$BUILD_DIR/simulator.o"
  "$CC" "${cflags_ufnet[@]}" -c "$ROOT/src/ufnet_core/cache_backend/redis.c" -o "$BUILD_DIR/redis.o"
  "$CC" "${cflags_ufnet[@]}" -c "$ROOT/src/ufnet_core/cache_backend/persistence.c" -o "$BUILD_DIR/persistence.o"

  "$CC" "${cflags_common[@]}" -o "$BUILD_DIR/ratelimitv2_simulator" \
    "$BUILD_DIR/simulator.o" "$BUILD_DIR/ratelimit.o" \
    "$BUILD_DIR/ratelimit_backend_memory.o" "$BUILD_DIR/ratelimit_backend_redis.o" \
    "$BUILD_DIR/redis.o" "$BUILD_DIR/persistence.o" \
    "${hiredis_objs[@]}" "$UFLIB_LIB" -L/opt/lib -lutf8proc -lm -lpthread
}

# ── Integration driver (live Redis; overrides the clock for `collapse`) ─────
build_integration() {
  echo "[run] building standalone integration driver"
  mapfile -t hiredis_objs < <(build_hiredis)
  build_ratelimit_obj
  build_ratelimit_redis_obj

  "$CC" "${cflags_ufnet[@]}" -c "$HERE/ratelimit_v2_driver.c" -o "$BUILD_DIR/integration_driver.o"
  "$CC" "${cflags_ufnet[@]}" -c "$ROOT/src/ufnet_core/cache_backend/redis.c" -o "$BUILD_DIR/redis.o"
  "$CC" "${cflags_ufnet[@]}" -c "$ROOT/src/ufnet_core/cache_backend/persistence.c" -o "$BUILD_DIR/persistence.o"

  # ratelimit.c -> RateLimitNowInMicros (clock_gettime, no uflib).  redis.c /
  # persistence.c -> mstrlcpy (utils_str.o) -> utf8proc; the installed uflib.pc
  # does not declare it (pre-existing gap), so link it explicitly.
  "$CC" "${cflags_common[@]}" -o "$BUILD_DIR/ratelimit_v2_driver" \
    "$BUILD_DIR/integration_driver.o" "$BUILD_DIR/ratelimit.o" \
    "$BUILD_DIR/ratelimit_backend_redis.o" \
    "$BUILD_DIR/redis.o" "$BUILD_DIR/persistence.o" \
    "${hiredis_objs[@]}" "$UFLIB_LIB" -L/opt/lib -lutf8proc -lm -lpthread
}

# ── Run ────────────────────────────────────────────────────────────────────
run_unit() {
  "$BUILD_DIR/ratelimit_v2_unit_driver" "$@"
}

run_memory() {
  "$BUILD_DIR/ratelimit_v2_test_memory" "$@"
}

run_sim() {
  "$BUILD_DIR/ratelimitv2_simulator" "$@"
}

run_integration() {
  "$HERE/ratelimit_v2_integration.sh" --scenario "$1" --driver "$BUILD_DIR/ratelimit_v2_driver"
}

case "$SCENARIO" in
  unit)
    build_unit
    run_unit
    ;;
  memory)
    build_memory
    run_memory
    ;;
  sim)
    build_sim
    run_sim
    ;;
  sim-redis)
    build_sim
    # The redis path leaks the hiredis connection at exit (matching the
    # integration driver), so disable leak detection for this run.
    ASAN_OPTIONS=detect_leaks=0 run_sim --backend redis --host "${RL_REDIS_HOST:-127.0.0.1}" --port "${RL_REDIS_PORT:-63793}"
    ;;
  all)
    build_unit
    build_memory
    build_sim
    build_integration
    run_unit
    echo
    run_memory
    echo
    run_sim
    echo
    for s in single concurrency collapse connect_down; do
      run_integration "$s"
    done
    ;;
  single|concurrency|collapse|connect_down)
    build_integration
    run_integration "$SCENARIO"
    ;;
esac

echo "[run] PASS: ratelimit_v2 test ($SCENARIO)"
