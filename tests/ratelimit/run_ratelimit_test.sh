#!/usr/bin/env bash
#
# One-invocation entry point for the ufnetcorelib ratelimit test suite.
#
# Builds two standalone drivers from the *production* translation units and runs
# them, deliberately NOT via the full ufnetcorelib cmake build (whose ufsrv_config
# composer emits stale stubs — see tests/redis/run_toxiproxy_redis_test.sh):
#
#   ratelimit_unit_driver    — deterministic, no live Redis. Substitutes the two
#                              boundaries with --wrap=RedisGetReply /
#                              --wrap=GetTimeNowInMicros and asserts the command
#                              sequence, status arithmetic, and fixed-defect
#                              regression behaviour.
#   ratelimit_driver         — live-Redis integration. Links ratelimit.c + redis.c
#                              + persistence.c + vendored hiredis + system uflib.
#
# Usage:
#   ./tests/ratelimit/run_ratelimit_test.sh             # unit + integration
#   ./tests/ratelimit/run_ratelimit_test.sh unit        # unit only
#   ./tests/ratelimit/run_ratelimit_test.sh single      # one integration scenario
#
# Exit 0 if every requested scenario passed, non-zero otherwise.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD_DIR="$ROOT/build/ratelimit-test"

UFLIB_LIB="${UFLIB_LIB:-/usr/local/lib/libuflib.a}"
UFLIB_INC="${UFLIB_INC:-/usr/local/include/uflib}"
CC="${CC:-clang}"

SCENARIO="${1:-all}"
case "$SCENARIO" in
  all|unit|single|concurrency|collapse|connect_down) ;;
  *) echo "usage: $0 [all|unit|single|concurrency|collapse|connect_down]" >&2; exit 2 ;;
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
  for src in async dict hiredis net sds sockcompat read sslio; do
    if [[ ! -f "$BUILD_DIR/hiredis_$src.o" ]]; then
      "$CC" -std=gnu17 -D_GNU_SOURCE -I"$ROOT/packages/hiredis" \
        -c "$ROOT/packages/hiredis/$src.c" -o "$BUILD_DIR/hiredis_$src.o"
    fi
    hiredis_objs+=("$BUILD_DIR/hiredis_$src.o")
  done
  printf '%s\n' "${hiredis_objs[@]}"
}

# ── ratelimit.o (shared between the two drivers) ───────────────────────────
build_ratelimit_obj() {
  mkdir -p "$BUILD_DIR"
  "$CC" "${cflags_ufnet[@]}" -c "$ROOT/src/ufnet_core/ratelimit/ratelimit.c" -o "$BUILD_DIR/ratelimit.o"
}

# ── Unit driver (no live Redis; wraps RedisGetReply + GetTimeNowInMicros) ──
build_unit() {
  echo "[run] building standalone unit driver"
  mapfile -t hiredis_objs < <(build_hiredis)
  build_ratelimit_obj

  "$CC" "${cflags_ufnet[@]}" -c "$HERE/ratelimit_unit_driver.c" -o "$BUILD_DIR/unit_driver.o"

  "$CC" "${cflags_common[@]}" -o "$BUILD_DIR/ratelimit_unit_driver" \
    "$BUILD_DIR/unit_driver.o" "$BUILD_DIR/ratelimit.o" \
    "${hiredis_objs[@]}" "$UFLIB_LIB" -lm -lpthread \
    -Wl,--wrap=RedisGetReply -Wl,--wrap=GetTimeNowInMicros
}

# ── Integration driver (live Redis; wraps GetTimeNowInMicros for `collapse`) ──
build_integration() {
  echo "[run] building standalone integration driver"
  mapfile -t hiredis_objs < <(build_hiredis)
  build_ratelimit_obj

  "$CC" "${cflags_ufnet[@]}" -c "$HERE/ratelimit_driver.c" -o "$BUILD_DIR/integration_driver.o"
  "$CC" "${cflags_ufnet[@]}" -c "$ROOT/src/ufnet_core/cache_backend/redis.c" -o "$BUILD_DIR/redis.o"
  "$CC" "${cflags_ufnet[@]}" -c "$ROOT/src/ufnet_core/cache_backend/persistence.c" -o "$BUILD_DIR/persistence.o"

  # ratelimit.c -> GetTimeNowInMicros (utils_time.o) -> mstrlcpy (utils_str.o)
  # -> utf8proc.  The installed uflib.pc does not declare it (pre-existing gap),
  # so link it explicitly.
  "$CC" "${cflags_common[@]}" -o "$BUILD_DIR/ratelimit_driver" \
    "$BUILD_DIR/integration_driver.o" "$BUILD_DIR/ratelimit.o" \
    "$BUILD_DIR/redis.o" "$BUILD_DIR/persistence.o" \
    "${hiredis_objs[@]}" "$UFLIB_LIB" -L/opt/lib -lutf8proc -lm -lpthread \
    -Wl,--wrap=GetTimeNowInMicros
}

# ── Run ────────────────────────────────────────────────────────────────────
run_unit() {
  "$BUILD_DIR/ratelimit_unit_driver" "$@"
}

run_integration() {
  "$HERE/ratelimit_integration.sh" --scenario "$1" --driver "$BUILD_DIR/ratelimit_driver"
}

case "$SCENARIO" in
  unit)
    build_unit
    run_unit
    ;;
  all)
    build_unit
    build_integration
    run_unit
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

echo "[run] PASS: ratelimit test ($SCENARIO)"
