#!/usr/bin/env bash
#
# Busy-session pipeline proof: one authenticated session pushes N commands
# without waiting for the previous completion. The server must hold the extra
# requests while it services the current one and bleed them afterwards, so
# every fence is created exactly once and every command is acknowledged.
#
# Evidence: the CLI's stdout events plus its --pending-report table, which
# records each command's dispatch and completion timestamps.
set -uo pipefail

CONTAINER="${CONTAINER:-ufsrvwebsock}"
PROCNAME="${PROCNAME:-ufsrvwebsock}"
CLI="${CLI:-/home/devops/repos/1.ufsrv/ufsrvcli/build/dev/src/ufsrvcli}"
PROFILES_DIR="${PROFILES_DIR:-}"
ACCOUNT="${ACCOUNT:-1}"
NGROUPS="${NGROUPS:-8}"
DEPTH="${DEPTH:-4}"
PREFIX="${PREFIX:-bkgpipe}"
EVIDENCE_DIR="${EVIDENCE_DIR:-/tmp/concurrency_correctness_proofs}"
TIMEOUT="${TIMEOUT:-60}"
LOG="${LOG:-/var/log/ufsrvwebsock.log}"
REDIS_SESSION="${REDIS_SESSION:-redis-session}"
REDIS_FENCE="${REDIS_FENCE:-redis-fence}"
LOCAL="${LOCAL:-0}"
REDIS_HOST="${REDIS_HOST:-127.0.0.1}"
SESSION_PORT="${SESSION_PORT:-63791}"
FENCE_PORT="${FENCE_PORT:-63792}"

usage() {
  cat <<EOF
Usage: $0 [options]

  --cli PATH            ufsrvcli binary
  --home-base DIR       numbered home bases; --account selects <DIR>/<account>
  --account K           signed-on account                  (default $ACCOUNT)
  --groups N            commands pushed without waiting    (default $NGROUPS)
  --depth N             CLI pipeline depth (--pipeline)    (default $DEPTH)
  --prefix NAME         fence name prefix                  (default $PREFIX)
  --evidence-dir DIR    run directory root                 (default $EVIDENCE_DIR)
  --timeout SECS        bound on the whole run             (default $TIMEOUT)
  --log PATH            server log in the container        (default $LOG)
  --container NAME      websock container                  (default $CONTAINER)
  --redis-session NAME  session store                      (default $REDIS_SESSION)
  --redis-fence NAME    fence store                        (default $REDIS_FENCE)
  --local               inspect host services; stores at \$REDIS_HOST
  -h, --help            this text
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --cli) CLI="$2"; shift 2 ;;
    --home-base) PROFILES_DIR="$2"; shift 2 ;;
    --account) ACCOUNT="$2"; shift 2 ;;
    --groups) NGROUPS="$2"; shift 2 ;;
    --depth) DEPTH="$2"; shift 2 ;;
    --prefix) PREFIX="$2"; shift 2 ;;
    --evidence-dir) EVIDENCE_DIR="$2"; shift 2 ;;
    --timeout) TIMEOUT="$2"; shift 2 ;;
    --log) LOG="$2"; shift 2 ;;
    --container) CONTAINER="$2"; shift 2 ;;
    --redis-session) REDIS_SESSION="$2"; shift 2 ;;
    --redis-fence) REDIS_FENCE="$2"; shift 2 ;;
    --local) LOCAL=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'unknown option: %s\n' "$1" >&2; usage; exit 2 ;;
  esac
done

die() { printf 'FAIL(preflight): %s\n' "$*" >&2; exit 2; }
for v in "$CLI" "$EVIDENCE_DIR" ${PROFILES_DIR:+"$PROFILES_DIR"}; do
  [ "${v#/}" = "$v" ] && die "path must be absolute: $v"
done
case "$NGROUPS$DEPTH" in *[!0-9]*) die "--groups and --depth must be positive integers" ;; esac
[ "$NGROUPS" -ge 2 ] || die "--groups must be at least 2"
[ "$DEPTH" -ge 1 ] || die "--depth must be at least 1"
[ "$DEPTH" -le "$NGROUPS" ] || die "--depth must not exceed --groups"
[ -n "$PROFILES_DIR" ] || die "--home-base is required"
[ -x "$CLI" ] || die "CLI not executable: $CLI"
HOMEDIR="$PROFILES_DIR/$ACCOUNT"
[ -r "$HOMEDIR/ufsrvcli.config" ] || die "client configuration is not readable: $HOMEDIR/ufsrvcli.config"
[ -r "$HOMEDIR/ufsrvcli.prefs" ] || die "client prefs is not readable: $HOMEDIR/ufsrvcli.prefs"

if [ "$LOCAL" = 1 ]; then
  [ -r "$LOG" ] || die "server log is not readable: $LOG"
  red() { redis-cli -h "$REDIS_HOST" -p "$SESSION_PORT" "$@"; }
  fencered() { redis-cli -h "$REDIS_HOST" -p "$FENCE_PORT" "$@"; }
else
  docker inspect "$CONTAINER" >/dev/null 2>&1 || die "no container '$CONTAINER'"
  docker inspect "$REDIS_FENCE" >/dev/null 2>&1 || die "no container '$REDIS_FENCE'"
  docker exec "$CONTAINER" test -f "$LOG" || die "no log at $LOG in $CONTAINER"
  red() { docker exec "$REDIS_SESSION" redis-cli "$@"; }
  fencered() { docker exec "$REDIS_FENCE" redis-cli "$@"; }
fi

RUNID="$(date +%Y%m%d%H%M%S)"
WORKDIR="$EVIDENCE_DIR/pipe_$RUNID"
mkdir -p "$WORKDIR/home" || die "cannot create $WORKDIR"
cp "$HOMEDIR/ufsrvcli.config" "$HOMEDIR/ufsrvcli.prefs" "$WORKDIR/home/" || die "cannot copy the client home"
chmod 600 "$WORKDIR/home/ufsrvcli.config" "$WORKDIR/home/ufsrvcli.prefs"
EVENTS="$WORKDIR/events.log"
REPORT="$WORKDIR/pending.tsv"
FEED="$WORKDIR/feed"
mkfifo "$FEED" || die "cannot create $FEED"

declare -a NAMES=() IDS=()
for i in $(seq 1 "$NGROUPS"); do
  NAMES+=("$(printf '%s_%s_%02d' "$PREFIX" "$RUNID" "$i")")
  IDS+=("cmd_$i")
done

printf '  run-id        : %s\n' "$RUNID"
printf '  account       : %s\n' "$ACCOUNT"
printf '  groups/depth  : %s commands, depth %s\n' "$NGROUPS" "$DEPTH"
printf '  names         : %s .. %s\n' "${NAMES[0]}" "${NAMES[$((NGROUPS - 1))]}"
printf '  evidence      : %s\n' "$WORKDIR"

if [ "$LOCAL" = 1 ]; then
  LOG_BEFORE=$(wc -l < "$LOG")
else
  LOG_BEFORE=$(docker exec "$CONTAINER" sh -c "wc -l < $LOG" | tr -d ' ')
fi

ASAN_OPTIONS=detect_leaks=0 "$CLI" --headless --command-stream --home-base "$WORKDIR/home" \
  --pipeline "$DEPTH" --pending-report "$REPORT" --session-timeout-ms $((TIMEOUT * 1000)) \
  < "$FEED" > "$EVENTS" 2>&1 &
CLI_PID=$!
exec 3> "$FEED"

deadline=$(( $(date +%s) + TIMEOUT ))
until grep -q '"event":"ready"' "$EVENTS" 2>/dev/null; do
  [ "$(date +%s)" -lt "$deadline" ] || die "timed out waiting for the CLI to become ready"
  sleep 0.1
done

for i in $(seq 0 $((NGROUPS - 1))); do
  printf '{"id":"%s","op":"join","name":"%s"}\n' "${IDS[$i]}" "${NAMES[$i]}" >&3
done

deadline=$(( $(date +%s) + TIMEOUT ))
while [ "$(grep -c '"event":"joined"' "$EVENTS" 2>/dev/null || true)" -lt "$NGROUPS" ]; do
  [ "$(date +%s)" -lt "$deadline" ] || { printf 'FAIL: timed out waiting for %s confirmations\n' "$NGROUPS"; break; }
  sleep 0.2
done
printf '{"id":"stop","op":"stop"}\n' >&3
exec 3>&-
wait "$CLI_PID"
CLI_RC=$?

ok=0
check() {
  local id="$1" what="$2" expected="$3" observed="$4"
  local verdict=PASS
  [ "$expected" = "$observed" ] || { verdict=FAIL; ok=1; }
  printf '  %-4s %-52s %-14s %-14s %s\n' "$id" "$what" "$expected" "$observed" "$verdict"
}

rows=$(awk -F'\t' 'NF>=10' "$REPORT" 2>/dev/null | wc -l | tr -d ' ')
completed=$(awk -F'\t' '$1=="completed"' "$REPORT" 2>/dev/null | wc -l | tr -d ' ')
joined=$(grep -c '"event":"joined"' "$EVENTS" || true)
errors=$(grep -c '"event":"error"' "$EVENTS" || true)
cids=$(grep -o '"cid":"[0-9]*"' "$EVENTS" | sort -u | wc -l | tr -d ' ')
fids=$(grep '"event":"joined"' "$EVENTS" | grep -o '"fid":"[0-9]*"' | sort -u | wc -l | tr -d ' ')
ids=$(grep '"event":"joined"' "$EVENTS" | grep -o '"id":"cmd_[0-9]*"' | sort -u | wc -l | tr -d ' ')
concurrency=$(awk -F'\t' '
  $1=="completed" { d[n]=$6; c[n]=$7; n++ }
  END {
    max=0
    for (i=0;i<n;i++) {
      active=0
      for (j=0;j<n;j++) if (d[j] <= d[i] && c[j] > d[i]) active++
      if (active>max) max=active
    }
    print max
  }' "$REPORT" 2>/dev/null)

printf '\n  id   invariant                                            expected       observed       verdict\n'
printf '  %s\n' '--------------------------------------------------------------------------------------------'
check C1 "commands recorded as dispatched" "$NGROUPS" "$rows"
check C2 "commands completed" "$NGROUPS" "$completed"
check C3 "joined confirmations received" "$NGROUPS" "$joined"
check C4 "distinct confirmations (no duplicates)" "$NGROUPS" "$fids"
check C5 "confirmations carry their own command id" "$NGROUPS" "$ids"
check C6 "one session for the whole run" "1" "$cids"
check C7 "CLI exited 0" "0" "$CLI_RC"
check C8 "no error events" "0" "$errors"
check C9 "pipelining observed (max in flight, at least depth)" "$DEPTH" "$concurrency"
[ "$concurrency" -ge "$DEPTH" ] || ok=1

missing=0
uid=$(grep -o '"uid":"[0-9]*"' "$EVENTS" | head -1 | tr -dc '0-9')
while IFS= read -r line; do
  fid=$(printf '%s' "$line" | grep -o '"fid":"[0-9]*"' | tr -dc '0-9')
  name=$(printf '%s' "$line" | sed -n "s/.*\"name\":\"\([^\"]*\)\".*/\1/p")
  [ -n "$fid" ] || { missing=$((missing + 1)); continue; }
  [ "$(fencered exists "BID:$fid" | tr -d '[:space:]')" = "1" ] || { printf '  fence store: %s -> %s MISSING\n' "$name" "$fid"; missing=$((missing + 1)); }
  members=$(red --raw ZRANGE "MEMBER_USERS_FOR_FENCE:$fid" 0 -1 | tr '\n' ' ')
  [ "$members" = "$uid " ] || { printf '  membership : %s -> %s expected %s\n' "$name" "$members" "$uid"; missing=$((missing + 1)); }
done < <(grep '"event":"joined"' "$EVENTS")

check C10 "fences present in the store with sole membership" "$NGROUPS" "$((NGROUPS - missing))"
[ "$missing" -eq 0 ] || ok=1

if [ "$LOCAL" = 1 ]; then
  tail -n +"$((LOG_BEFORE + 1))" "$LOG" > "$WORKDIR/server.log"
else
  docker exec "$CONTAINER" sh -c "tail -n +$((LOG_BEFORE + 1)) $LOG" > "$WORKDIR/server.log"
fi
queued=$(grep -c "MESSAGE QUEUE: Added to queue without decoding" "$WORKDIR/server.log" || true)
drained=$(grep -c "Reading/Decoding directly from SocketMessage buffer" "$WORKDIR/server.log" || true)
lockfail=$(grep -c "COULD NOT ACQUIRE TRY-READ LOCK" "$WORKDIR/server.log" || true)

printf '\n  server log evidence (%s, %s lines captured)\n' "$LOG" "$(wc -l < "$WORKDIR/server.log" | tr -d ' ')"
printf '    queue-add without session lock (e:912)   %s\n' "$queued"
printf '    consolidate/read from queued buffer      %s\n' "$drained"
printf '    session lock try-read failures           %s\n' "$lockfail"
if [ "$queued" -eq 0 ]; then
  printf '    NOTE: no socket-read queueing in this window; the pipelined commands travelled the REST/intra path\n'
fi

if [ "$ok" -eq 0 ]; then
  printf '\n  PASS — %s commands pushed at depth %s, all confirmed once, all fences stored\n' "$NGROUPS" "$DEPTH"
  printf '  evidence: %s\n' "$WORKDIR"
  exit 0
fi
printf '\n  FAIL — evidence: %s\n' "$WORKDIR"
exit 1
