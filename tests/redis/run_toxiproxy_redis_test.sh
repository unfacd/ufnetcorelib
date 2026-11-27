#!/usr/bin/env bash
#
# One-invocation entry point for the Toxiproxy Redis-reconnect harness.
#
# Builds the standalone driver (redis.c + persistence.c + vendored hiredis +
# uflib — deliberately NOT the full ufnetcorelib library, whose ufsrv_config
# composer is broken), then runs the requested scenarios end to end against a
# scratch Redis fronted by Toxiproxy.
#
# Usage:
#   ./tests/redis/run_toxiproxy_redis_test.sh                # all scenarios
#   ./tests/redis/run_toxiproxy_redis_test.sh exhaustion     # one scenario
#
# Exit 0 if every requested scenario passed, non-zero otherwise.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD_DIR="$ROOT/build/redis-test"

UFLIB_LIB="${UFLIB_LIB:-/usr/local/lib/libuflib.a}"
UFLIB_INC="${UFLIB_INC:-/usr/local/include/uflib}"
CC="${CC:-clang}"

SCENARIO="${1:-all}"
case "$SCENARIO" in
  all|connect_down|exhaustion|recovery|recovery_depth|store_result_loss) ;;
  *) echo "usage: $0 [all|connect_down|exhaustion|recovery|recovery_depth|store_result_loss]" >&2; exit 2 ;;
esac

# ── 1. Build the standalone driver ──────────────────────────────────────────
# The full ufnetcorelib build is broken (ufsrv_config composer emits stale
# stubs), so we compile just the two cache-backend translation units we need
# and link them against the vendored hiredis + system uflib.  uflib is a
# sanitizer (dev-preset) install, so everything is built with ASan+UBSan.
build_driver() {
  mkdir -p "$BUILD_DIR"

  local inc_ufnet=(
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
  local cflags_common=(-std=gnu17 -D_GNU_SOURCE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
  local cflags_ufnet=("${cflags_common[@]}" -fblocks -include stdatomic.h -include config_defs_ufnetcorelib.h "${inc_ufnet[@]}")

  # vendored hiredis (synchronous core + harmless async/ssl stubs)
  local hiredis_objs=()
  local src
  for src in async dict hiredis net sds sockcompat read sslio; do
    "$CC" -std=gnu17 -D_GNU_SOURCE -I"$ROOT/packages/hiredis" \
      -c "$ROOT/packages/hiredis/$src.c" -o "$BUILD_DIR/hiredis_$src.o"
    hiredis_objs+=("$BUILD_DIR/hiredis_$src.o")
  done

  "$CC" "${cflags_ufnet[@]}" -c "$HERE/redis_reconnect_driver.c" -o "$BUILD_DIR/driver.o"
  "$CC" "${cflags_ufnet[@]}" -c "$ROOT/src/ufnet_core/cache_backend/redis.c" -o "$BUILD_DIR/redis.o"
  "$CC" "${cflags_ufnet[@]}" -c "$ROOT/src/ufnet_core/cache_backend/persistence.c" -o "$BUILD_DIR/persistence.o"

  "$CC" "${cflags_common[@]}" -o "$BUILD_DIR/redis_reconnect_driver" \
    "$BUILD_DIR/driver.o" "$BUILD_DIR/redis.o" "$BUILD_DIR/persistence.o" \
    "${hiredis_objs[@]}" "$UFLIB_LIB" -lm -lpthread -Wl,--wrap=nanosleep
}

echo "[run] building standalone driver"
build_driver

# ── 2. Run the scenario(s) ─────────────────────────────────────────────────
run_one() {
  "$HERE/toxiproxy_redis_reconnect.sh" --scenario "$1" --driver "$BUILD_DIR/redis_reconnect_driver"
}

if [ "$SCENARIO" = "all" ]; then
  run_one connect_down
  run_one exhaustion
  run_one recovery
  run_one recovery_depth
  run_one store_result_loss
else
  run_one "$SCENARIO"
fi

echo "[run] PASS: toxiproxy redis reconnect test ($SCENARIO)"
