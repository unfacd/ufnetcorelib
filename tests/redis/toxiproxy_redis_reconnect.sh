#!/usr/bin/env bash
#
# Orchestrates the Toxiproxy Layer-3 harness for the Redis connection-loss
# retry loop (see dev/technical_designs/UFLIB_TESTING_REDIS.md).
#
# Provisions a scratch Redis and a Toxiproxy v2.12.0 server, creates a proxy in
# front of the scratch Redis, runs the standalone driver against the proxy
# front, and injects a fault mid-command.  Over the proxy the fault surfaces as
# a connection loss, which redis.c now maps into an exponential-backoff
# reconnect loop (1000 -> 2000 -> 4000 -> 8000 -> 10000 ms) that returns NULL /
# REDIS_ERR on exhaustion instead of exit(2).
#
# Why a host binary, not the Docker image: the shopify/toxiproxy image on
# Docker Hub is frozen at 2.1.4 (2019), which lacks reset_peer.  The current
# release (v2.12.0) ships prebuilt linux-amd64 binaries, so we download and run
# that directly.
#
# Usage:
#   toxiproxy_redis_reconnect.sh [--scenario connect_down|exhaustion|recovery] [--driver PATH] [--keep-infra]
#
# Exit 0 on success (driver returned the expected result), non-zero otherwise.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

API="http://127.0.0.1:8474"
PROXY_NAME="uflib-redis"
PROXY_LISTEN="127.0.0.1:63794"
SCRATCH_NAME="uflib-scratch-redis"
SCRATCH_HOST_PORT="63795"
TOXIC_NAME="reset_mid"
TOXIPROXY_VERSION="2.12.0"
TOXIPROXY_BIN="${TOXIPROXY_BIN:-/tmp/toxiproxy-server}"

SCENARIO="exhaustion"
DRIVER="${DRIVER:-$SCRIPT_DIR/redis_reconnect_driver}"
KEEP_INFRA=0
STARTED_TOXI=0
TOXIPROXY_PID=""
OUT=""

log() { printf '\033[1;34m[toxiproxy]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[toxiproxy] ERROR: %s\033[0m\n' "$*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --scenario) SCENARIO="$2"; shift 2 ;;
    --driver)   DRIVER="$2"; shift 2 ;;
    --keep-infra) KEEP_INFRA=1; shift ;;
    *) die "unknown argument: $1" ;;
  esac
done

case "$SCENARIO" in
  connect_down|exhaustion|recovery|recovery_depth|store_result_loss) ;;
  *) die "scenario must be one of: connect_down, exhaustion, recovery, recovery_depth, store_result_loss" ;;
esac
[[ -x "$DRIVER" ]] || die "driver not found/executable: $DRIVER (build it first)"

# Tear down whatever this run provisioned, on any exit path (success, die, or
# signal).  Idempotent — removing a non-existent container/process is a no-op.
cleanup() {
  curl -sf -X DELETE "$API/proxies/$PROXY_NAME/toxics/$TOXIC_NAME" >/dev/null 2>&1 || true
  curl -sf -X DELETE "$API/proxies/$PROXY_NAME" >/dev/null 2>&1 || true

  if [[ "$KEEP_INFRA" -eq 0 ]]; then
    log "tearing down infra (use --keep-infra to keep it)"
    sudo docker rm -f "$SCRATCH_NAME" >/dev/null 2>&1 || true
    if [[ "$STARTED_TOXI" -eq 1 ]]; then
      kill "${TOXIPROXY_PID:-}" 2>/dev/null || true
    fi
  fi

  if [[ -n "${OUT:-}" ]]; then
    rm -f "$OUT"
  fi
}
trap cleanup EXIT

# ── 1. Provision scratch Redis ─────────────────────────────────────────────
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

# ── 2. Provision Toxiproxy v2.12.0 (host binary — Docker image is stale) ──
if [ ! -x "$TOXIPROXY_BIN" ]; then
  log "downloading toxiproxy v$TOXIPROXY_VERSION server binary"
  curl -sSL -o "$TOXIPROXY_BIN" \
    "https://github.com/Shopify/toxiproxy/releases/download/v${TOXIPROXY_VERSION}/toxiproxy-server-linux-amd64" \
    || die "failed to download toxiproxy server"
  chmod +x "$TOXIPROXY_BIN"
fi

STARTED_TOXI=0
if ! curl -sf "$API/version" >/dev/null 2>&1; then
  log "starting toxiproxy v$TOXIPROXY_VERSION (API $API)"
  "$TOXIPROXY_BIN" -host 127.0.0.1 -port 8474 >/tmp/toxiproxy-server.log 2>&1 &
  TOXIPROXY_PID=$!
  STARTED_TOXI=1
fi
log "waiting for toxiproxy API"
for _ in $(seq 1 30); do
  if curl -sf "$API/version" >/dev/null 2>&1; then break; fi
  sleep 1
done
curl -sf "$API/version" >/dev/null 2>&1 || die "toxiproxy API not reachable"

# ── 3. Create the proxy (idempotent) ───────────────────────────────────────
log "creating proxy $PROXY_NAME: $PROXY_LISTEN -> 127.0.0.1:$SCRATCH_HOST_PORT"
curl -sf -X POST "$API/proxies" -H 'Content-Type: application/json' \
  -d "{\"name\":\"$PROXY_NAME\",\"listen\":\"$PROXY_LISTEN\",\"upstream\":\"127.0.0.1:$SCRATCH_HOST_PORT\",\"enabled\":true}" \
  >/dev/null || true
curl -sf -X DELETE "$API/proxies/$PROXY_NAME/toxics/$TOXIC_NAME" >/dev/null 2>&1 || true

# ── 4. Run the scenario ────────────────────────────────────────────────────
OUT="$(mktemp)"

DRIVER_ARGS=(--host 127.0.0.1 --port 63794 --label uflib-test --expect "$SCENARIO")

case "$SCENARIO" in
  connect_down)
    # Take the endpoint down so the driver's connect fails cleanly (no retry).
    log "disabling proxy (connect_down)"
    curl -sf -X POST "$API/proxies/$PROXY_NAME" -d '{"enabled":false}' >/dev/null \
      || die "failed to disable proxy"
    ;;
esac

log "running driver (scenario=$SCENARIO)"
ASAN_OPTIONS=detect_leaks=0 "$DRIVER" "${DRIVER_ARGS[@]}" >"$OUT" 2>&1 &
DRIVER_PID=$!

# Fault injection applies only to the connection-loss scenarios.
if [[ "$SCENARIO" == "exhaustion" || "$SCENARIO" == "store_result_loss" ]]; then
  for _ in $(seq 1 60); do grep -q READY "$OUT" 2>/dev/null && break; sleep 0.2; done
  grep -q READY "$OUT" || { cat "$OUT"; die "driver never became READY"; }

  log "injecting reset_peer toxic (persistent)"
  curl -sf -X POST "$API/proxies/$PROXY_NAME/toxics" -H 'Content-Type: application/json' \
    -d "{\"name\":\"$TOXIC_NAME\",\"type\":\"reset_peer\",\"stream\":\"downstream\",\"attributes\":{\"timeout\":0}}" \
    >/dev/null || die "failed to add reset_peer toxic"

elif [[ "$SCENARIO" == "recovery" || "$SCENARIO" == "recovery_depth" ]]; then
  for _ in $(seq 1 60); do grep -q READY "$OUT" 2>/dev/null && break; sleep 0.2; done
  grep -q READY "$OUT" || { cat "$OUT"; die "driver never became READY"; }

  # Transient outage: disable the proxy (FIN to the in-flight command), then
  # re-enable it within the first backoff (recovery, 300 ms) or after the 3rd
  # backoff (recovery_depth, ~8 s) so the reconnect succeeds at a deeper depth.
  reenable_after=0.3
  if [[ "$SCENARIO" == "recovery_depth" ]]; then
    reenable_after=8
  fi
  log "disabling proxy (transient outage), re-enabling after ${reenable_after}s"
  curl -sf -X POST "$API/proxies/$PROXY_NAME" -d '{"enabled":false}' >/dev/null \
    || die "failed to disable proxy"
  sleep "$reenable_after"
  curl -sf -X POST "$API/proxies/$PROXY_NAME" -d '{"enabled":true}' >/dev/null \
    || die "failed to re-enable proxy"
fi

set +e
wait "$DRIVER_PID"
RC=$?
set -e
log "driver exited rc=$RC"
echo "---- driver output ----"
cat "$OUT"
echo "-----------------------"

exit "$RC"
