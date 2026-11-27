#!/usr/bin/env bash
#
# Provisions a scratch Redis and runs the ratelimit_v2 integration driver
# against it.  No Toxiproxy is needed: this suite asserts the *distributed
# sliding-window* path, not connection-loss recovery.  `connect_down` is
# exercised against a dead port.
#
# Usage:
#   ratelimit_v2_integration.sh --scenario single|concurrency|collapse|connect_down \
#       --driver /path/to/ratelimit_v2_driver [--keep-infra]
#
# Exit 0 on success (driver returned the expected result), non-zero otherwise.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SCRATCH_NAME="uflib-scratch-redis-rlv2"
SCRATCH_HOST_PORT="63796"
DEAD_PORT="63999"

SCENARIO="single"
DRIVER="${DRIVER:-$SCRIPT_DIR/ratelimit_v2_driver}"
KEEP_INFRA=0
OUT=""

log() { printf '\033[1;34m[ratelimit_v2]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[ratelimit_v2] ERROR: %s\033[0m\n' "$*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --scenario) SCENARIO="$2"; shift 2 ;;
    --driver)   DRIVER="$2"; shift 2 ;;
    --keep-infra) KEEP_INFRA=1; shift ;;
    *) die "unknown argument: $1" ;;
  esac
done

case "$SCENARIO" in
  single|concurrency|collapse|connect_down) ;;
  *) die "scenario must be one of: single, concurrency, collapse, connect_down" ;;
esac
[[ -x "$DRIVER" ]] || die "driver not found/executable: $DRIVER (build it first)"

cleanup() {
  if [[ "$KEEP_INFRA" -eq 0 ]]; then
    log "tearing down scratch Redis (use --keep-infra to keep it)"
    sudo docker rm -f "$SCRATCH_NAME" >/dev/null 2>&1 || true
  fi
  if [[ -n "${OUT:-}" ]]; then
    rm -f "$OUT"
  fi
}
trap cleanup EXIT

# ── Provision scratch Redis ────────────────────────────────────────────────
if [[ "$SCENARIO" != "connect_down" ]]; then
  if ! sudo docker ps --format '{{.Names}}' | grep -qx "$SCRATCH_NAME"; then
    log "starting scratch Redis ($SCRATCH_NAME :$SCRATCH_HOST_PORT)"
    sudo docker run -d --name "$SCRATCH_NAME" \
      -p "127.0.0.1:${SCRATCH_HOST_PORT}:6379" \
      redis:6.2.22-alpine >/dev/null
  fi
  log "waiting for scratch Redis to accept connections"
  for _ in $(seq 1 30); do
    if sudo docker exec "$SCRATCH_NAME" redis-cli ping >/dev/null 2>&1; then
      break
    fi
    sleep 1
  done
  sudo docker exec "$SCRATCH_NAME" redis-cli ping >/dev/null 2>&1 \
    || die "scratch Redis did not become ready"

  log "flushing scratch Redis"
  sudo docker exec "$SCRATCH_NAME" redis-cli flushall >/dev/null
fi

# ── Run the driver ─────────────────────────────────────────────────────────
OUT="$(mktemp)"

if [[ "$SCENARIO" == "connect_down" ]]; then
  HOST_PORT="$DEAD_PORT"
else
  HOST_PORT="$SCRATCH_HOST_PORT"
fi

log "running driver (scenario=$SCENARIO, port=$HOST_PORT)"
ASAN_OPTIONS=detect_leaks=0 "$DRIVER" \
  --host 127.0.0.1 --port "$HOST_PORT" --label uflib-rl --scenario "$SCENARIO" >"$OUT" 2>&1
RC=$?

log "driver exited rc=$RC"
echo "---- driver output ----"
cat "$OUT"
echo "-----------------------"

exit "$RC"
