#!/usr/bin/env bash
set -uo pipefail

CONTAINER="${CONTAINER:-ufsrvwebsock}"
SERVICE="${SERVICE:-ufsrvwebsock@1}"
PROCNAME="${PROCNAME:-ufsrvwebsock}"
CLI="${CLI:-/home/devops/repos/1.ufsrv/ufsrvcli/build/dev/src/ufsrvcli}"
PREFS="${PREFS:-/tmp/mu/ccp_full.prefs}"
PROFILES_DIR="${PROFILES_DIR:-}"
ACCOUNT="${ACCOUNT:-1}"
NGROUPS="${NGROUPS:-10}"
WORKERS="${WORKERS:-}"
PREFIX="${PREFIX:-bkg}"
LOG="${LOG:-/var/log/ufsrvwebsock.log}"
EVIDENCE_DIR="${EVIDENCE_DIR:-/tmp/concurrency_correctness_proofs}"
REDIS_SESSION="${REDIS_SESSION:-redis-session}"
REDIS_FENCE="${REDIS_FENCE:-redis-fence}"
FENCE_CLASS="${FENCE_CLASS:-ufsrvwebsock}"
FENCE_GEOGROUP="${FENCE_GEOGROUP:-3}"
FENCE_INSTANCE="${FENCE_INSTANCE:-1}"
TIMEOUT="${TIMEOUT:-90}"
APIC="${APIC:-ufsrvapi}"
APILOG="${APILOG:-/var/log/ufsrvapi.log}"
RESTART=0
API_ONLY=0
LOCAL="${LOCAL:-0}"
REDIS_HOST="${REDIS_HOST:-127.0.0.1}"
SESSION_PORT="${SESSION_PORT:-63791}"
FENCE_PORT="${FENCE_PORT:-63792}"
PRINT_INVARIANTS=0
SEED="${SEED:-20261108}"

usage() {
  cat <<EOF
Usage: $0 [options]        (every path is absolute; flags override env override default)

  --cli PATH            ufsrvcli binary          (default $CLI)
  --prefs PATH          authenticated prefs      (default $PREFS)
  --home-base DIR       numbered home bases: use <DIR>/<account>/ufsrvcli.prefs
                        (config and prefs; overrides --prefs; --account selects k, default $ACCOUNT)
  --account K[,K..]     client accounts: each entry is a home base under --home-base and signs
                        on with its own cookie (default $ACCOUNT)
  --groups N            group names in one invocation (default $NGROUPS)
  --workers K           concurrent CLI processes (default: one per client). Fewer workers than
                        clients share the clients out; more workers gives one worker per client
  --prefix NAME         name prefix              (default $PREFIX)
  --evidence PATH       captured log window      (default $EVIDENCE_DIR/evidence_bulk_groups.log)
  --log PATH            server log in container  (default $LOG)
  --container NAME      container                (default $CONTAINER)
  --redis-session NAME  config/instance store    (default $REDIS_SESSION)
  --redis-fence NAME    fence store              (default $REDIS_FENCE)
  --timeout SECS        bound on the CLI run     (default $TIMEOUT)
  --api-container NAME  api container (routing)  (default $APIC)
  --api-log PATH        api log in that container (default $APILOG)
  --api-only            verify HTTP submission and matching API publish logs;
                        does not verify WebSocket confirmation or Redis mutations
  --print-invariants    print the pre-run context and the invariant catalogue, then run
  --force-server-start  restart the service before the run (default: use the running server)
  --local               run against servers on this host: no docker exec, logs read locally,
                        stores at $REDIS_HOST:$SESSION_PORT (session) and $REDIS_HOST:$FENCE_PORT (fence)
                        CLI config comes from each numbered home base (--home-base required)
  -h, --help            this text
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --cli)              CLI="$2"; shift 2 ;;
    --prefs)            PREFS="$2"; shift 2 ;;
    --home-base)        PROFILES_DIR="$2"; shift 2 ;;
    --account)          ACCOUNT="$2"; shift 2 ;;
    --groups)           NGROUPS="$2"; shift 2 ;;
    --workers)          WORKERS="$2"; shift 2 ;;
    --prefix)           PREFIX="$2"; shift 2 ;;
    --evidence)         EVIDENCE="$2"; shift 2 ;;
    --log)              LOG="$2"; shift 2 ;;
    --container)        CONTAINER="$2"; shift 2 ;;
    --redis-session)    REDIS_SESSION="$2"; shift 2 ;;
    --redis-fence)      REDIS_FENCE="$2"; shift 2 ;;
    --timeout)          TIMEOUT="$2"; shift 2 ;;
    --api-container)    APIC="$2"; shift 2 ;;
    --api-log)          APILOG="$2"; shift 2 ;;
    --print-invariants) PRINT_INVARIANTS=1; shift ;;
    --force-server-start) RESTART=1; shift ;;
    --api-only)         API_ONLY=1; shift ;;
    --local)            LOCAL=1; shift ;;
    -h|--help)          usage; exit 0 ;;
    *)                  printf 'unknown option: %s\n' "$1" >&2; usage; exit 2 ;;
  esac
done

EVIDENCE="${EVIDENCE:-$EVIDENCE_DIR/evidence_bulk_groups.log}"
[ -n "$PROFILES_DIR" ] && PREFS="$PROFILES_DIR/$ACCOUNT/ufsrvcli.prefs"
for v in "$CLI" "$PREFS" "$EVIDENCE" ${PROFILES_DIR:+"$PROFILES_DIR"}; do
  [ "${v#/}" = "$v" ] && { printf 'FAIL(preflight): path must be absolute: %s\n' "$v" >&2; exit 2; }
done
case "$NGROUPS" in ''|*[!0-9]*) printf 'FAIL(preflight): --groups must be a positive integer\n' >&2; exit 2 ;; esac
[ "$NGROUPS" -ge 1 ] || { printf 'FAIL(preflight): --groups must be at least 1\n' >&2; exit 2; }
declare -a CLIENTS=()
IFS=',' read -r -a CLIENTS <<< "$ACCOUNT"
CLIENT_COUNT=${#CLIENTS[@]}
[ "$CLIENT_COUNT" -ge 1 ] || { printf 'FAIL(preflight): --account needs at least one account\n' >&2; exit 2; }
for c in "${CLIENTS[@]}"; do
  case "$c" in ''|*[!0-9]*|0*) printf 'FAIL(preflight): --account entries must be positive integers: %s\n' "$c" >&2; exit 2 ;; esac
done
[ "$(printf '%s\n' "${CLIENTS[@]}" | sort -u | wc -l)" = "$CLIENT_COUNT" ] || { printf 'FAIL(preflight): --account lists a duplicate client\n' >&2; exit 2; }
if [ "$CLIENT_COUNT" -gt 1 ] && [ -z "$PROFILES_DIR" ]; then
  printf 'FAIL(preflight): multiple clients need --home-base DIR: each client signs on with its own cookie\n' >&2; exit 2
fi
if [ -n "$WORKERS" ]; then
  case "$WORKERS" in ''|*[!0-9]*|0*) printf 'FAIL(preflight): --workers must be a positive integer\n' >&2; exit 2 ;; esac
  REQUESTED_WORKERS=$WORKERS
else
  REQUESTED_WORKERS=$CLIENT_COUNT
fi
if [ "$CLIENT_COUNT" -eq 1 ]; then
  MODE=single
  PARTICIPANTS=${WORKERS:-1}
  [ "$PARTICIPANTS" -le "$NGROUPS" ] || PARTICIPANTS=$NGROUPS
  WORKERS=$PARTICIPANTS
else
  MODE=multi
  PARTICIPANTS=$CLIENT_COUNT
  [ "$PARTICIPANTS" -le "$NGROUPS" ] || { printf 'FAIL(preflight): --groups must be at least the number of clients: %s\n' "$CLIENT_COUNT" >&2; exit 2; }
  WORKERS=${WORKERS:-$CLIENT_COUNT}
  [ "$WORKERS" -le "$CLIENT_COUNT" ] || WORKERS=$CLIENT_COUNT
  [ "$WORKERS" -le "$NGROUPS" ] || WORKERS=$NGROUPS
fi
if [ "$API_ONLY" = 1 ] && [ "$MODE" = single ] && [ "$PARTICIPANTS" -gt 1 ]; then
  printf 'FAIL(preflight): --api-only cannot exercise refused sign-ons: use one worker, or several clients\n' >&2; exit 2
fi

mkdir -p "$(dirname "$EVIDENCE")" || exit 2
CATALOGUE="$(dirname "$EVIDENCE")/invariants_bulk_groups.tsv"

say() { printf '\n== %s\n' "$*"; }
die() { printf 'FAIL(preflight): %s\n' "$*" >&2; exit 2; }

if [ "$LOCAL" = 1 ] && [ -z "$PROFILES_DIR" ]; then
  die "--local requires --home-base DIR with client configurations"
fi
cexec() { docker exec "$CONTAINER" sh -c "$1"; }
red() { docker exec "$REDIS_SESSION" redis-cli "$@"; }
fencered() { docker exec "$REDIS_FENCE" redis-cli "$@"; }
apec() { docker exec "$APIC" sh -c "$1"; }
if [ "$LOCAL" = 1 ]; then
  cexec() { sh -c "$1"; }
  apec() { sh -c "$1"; }
  red() { redis-cli -h "$REDIS_HOST" -p "$SESSION_PORT" "$@"; }
  fencered() { redis-cli -h "$REDIS_HOST" -p "$FENCE_PORT" "$@"; }
fi

RUNID="$(date +%Y%m%d%H%M%S)"
declare -a NAMES=()
for i in $(seq 1 "$NGROUPS"); do
  NAMES+=("$(printf '%s_%s_%02d' "$PREFIX" "$RUNID" "$i")")
done
LIST=$(IFS=,; printf '%s' "${NAMES[*]}")
declare -a CLIENT_LIST=() CLIENT_PREFS=() CLIENT_HOME=() CLIENT_LOGBASE=() CLIENT_ACCOUNT=()
if [ "$MODE" = single ]; then
  for t in $(seq 1 "$PARTICIPANTS"); do CLIENT_LIST[$t]="$LIST"; done
else
  for t in $(seq 1 "$PARTICIPANTS"); do CLIENT_LIST[$t]=""; done
  for i in $(seq 1 "$NGROUPS"); do
    t=$(( ((i - 1) % PARTICIPANTS) + 1 ))
    CLIENT_LIST[$t]+="${CLIENT_LIST[$t]:+,}${NAMES[$((i - 1))]}"
  done
fi

write_catalogue() {
  if [ "$MODE" = single ]; then
    C4_EXPECT="1 accepted, $((PARTICIPANTS - 1)) refused"
    C12_EXPECT="1"
  else
    C4_EXPECT="0"
    C12_EXPECT="$PARTICIPANTS"
  fi
  cat > "$CATALOGUE" <<'EOF'
C1	groups requested == POSTs observed	equal
C2	confirmations for the expected names	N
C3	confirmations for unexpected names	0
C4	CLI exit status	@C4@
C5	websock broadcasts for the expected names	N
C6	websock broadcasts for unexpected names	0
C7	create-lookup (COULD NOT FIND) precedes each broadcast	N
C8	confirmed fids present in the fence store	N
C9	routing retries bounded by one per request	true
C10	fence errors in the window	0
C11	instance activity stamp fresh at run start	true
C12	participants that signed on and honoured their whole list	@C12@
C13	fences whose sole member is the submitting user	N
C14	fence hashes match confirmation and creator	N
C15	fences present in the canonical name registry	N
C16	fences with all required search index entries	N
C17	fences indexed at the stored coordinates	N
C18	creator's fence index matches reverse membership score	N
C19	confirmations attributed to the submitting account	N
C20	duplicate creations for one name	0
EOF
  sed -i "s/@C4@/$C4_EXPECT/; s/@C12@/$C12_EXPECT/" "$CATALOGUE"
}

print_context() {
  printf '\n-- pre-run context (bulk group creation)\n'
  printf '  %-16s %s\n' container "$CONTAINER"
  printf '  %-16s %s\n' service "$SERVICE"
  printf '  %-16s %s\n' cli "$CLI"
  printf '  %-16s %s\n' clients "$(IFS=,; printf '%s' "${CLIENTS[*]}")"
  printf '  %-16s %s\n' mode "$MODE"
  printf '  %-16s %s\n' workers "$WORKERS (participants: $PARTICIPANTS)"
  printf '  %-16s %s\n' log "$LOG"
  printf '  %-16s %s\n' evidence "$EVIDENCE"
  printf '  %-16s %s\n' groups "$NGROUPS"
  printf '  %-16s %s\n' prefix "$PREFIX"
  printf '  %-16s %s\n' run-id "$RUNID"
  printf '  %-16s %s\n' names "$LIST"
  printf '  %-16s %s:%s:%s\n' fence-store "$FENCE_CLASS" "$FENCE_GEOGROUP" "$FENCE_INSTANCE"
  printf '  %-16s %s\n' 'server start' "$([ "$RESTART" = 1 ] && echo 'restart' || echo 'use running')"
  printf '\n-- invariant catalogue (%s)\n\n' "$CATALOGUE"
  printf '  %-6s %-56s %s\n' id invariant expected
  printf '  %s\n' "--------------------------------------------------------------------------------"
  while IFS=$'\t' read -r id what expect; do
    printf '  %-6s %-56s %s\n' "$id" "$what" "$expect"
  done < "$CATALOGUE"
}

write_catalogue
if [ "$PRINT_INVARIANTS" = 1 ] && [ "$API_ONLY" = 0 ]; then print_context; fi

say "input context"
[ -x "$CLI" ] || die "CLI not executable: $CLI"
if [ "$API_ONLY" = 1 ]; then
  [ "$RESTART" = 0 ] || die "--api-only cannot restart websock"
elif [ "$LOCAL" = 1 ]; then
  [ "$RESTART" = 0 ] || die "--force-server-start is not available with --local (start the servers yourself)"
else
  docker inspect "$CONTAINER" >/dev/null 2>&1 || die "no container '$CONTAINER'"
fi
if [ "$LOCAL" = 1 ]; then
  [ "$API_ONLY" = 1 ] || [ -r "$LOG" ] || die "local log is not readable: $LOG"
  [ -r "$APILOG" ] || die "local API log is not readable: $APILOG"
fi
if [ "$LOCAL" != 1 ] && [ "$API_ONLY" = 0 ]; then
  cexec "test -f $LOG" || die "no log at $LOG in $CONTAINER"
fi
for t in $(seq 1 "$PARTICIPANTS"); do
  if [ "$MODE" = single ]; then account="${CLIENTS[0]}"; else account="${CLIENTS[$((t - 1))]}"; fi
  CLIENT_ACCOUNT[t]="$account"
  if [ -n "$PROFILES_DIR" ]; then
    p="$PROFILES_DIR/$account/ufsrvcli.prefs"
    CLIENT_HOME[t]="$PROFILES_DIR/$account"
    [ -r "${CLIENT_HOME[t]}/ufsrvcli.config" ] || die "client configuration is not readable: ${CLIENT_HOME[t]}/ufsrvcli.config"
  else
    p="$PREFS"
  fi
  [ -f "$p" ] || die "no prefs file: $p"
  grep -q 'cookie' "$p" || die "prefs has no session.cookie: $p"
  grep -qE 'ufsrvuid[[:space:]]*=[[:space:]]*"[^"]' "$p" || die "prefs has no ufsrvuid: auth will fall back to a stale cookie ($p)"
  CLIENT_PREFS[t]="$p"
  printf '  worker %-2s     : account %-3s %s\n' "$t" "$account" "$p"
done
if [ "$MODE" = multi ]; then
  seen_cookies=""
  for t in $(seq 1 "$PARTICIPANTS"); do
    cookie=$(sed -n 's/.*cookie[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "${CLIENT_PREFS[t]}" | head -1)
    case "$seen_cookies" in *"|$cookie|"*) die "two clients reuse one session cookie: concurrent sign-ons with one cookie are refused" ;; esac
    seen_cookies="$seen_cookies|$cookie|"
  done
fi
printf '  groups        : %s (%s .. %s)\n' "$NGROUPS" "${NAMES[0]}" "${NAMES[$((NGROUPS - 1))]}"
if [ "$MODE" = single ]; then
  printf '  workers       : %s concurrent sign-ons on one cookie (account %s)\n' "$PARTICIPANTS" "${CLIENTS[0]}"
else
  printf '  clients       : %s accounts across %s workers\n' "$PARTICIPANTS" "$WORKERS"
fi

WORKDIR="$EVIDENCE_DIR/cli_$RUNID"
mkdir -p "$WORKDIR" || exit 2

CLI_LOG="$WORKDIR/ufsrvcli.log"
: > "$CLI_LOG"
for t in $(seq 1 "$PARTICIPANTS"); do
  mkdir -p "$WORKDIR/cli_$t" || exit 2
  if [ -n "$PROFILES_DIR" ]; then
    worker_home="$WORKDIR/cli_$t/${CLIENT_ACCOUNT[t]}"
    mkdir -m 700 "$worker_home" || exit 2
    cp "${CLIENT_HOME[t]}/ufsrvcli.config" "${CLIENT_PREFS[t]}" "$worker_home/" || exit 2
    chmod 600 "$worker_home/ufsrvcli.config" "$worker_home/ufsrvcli.prefs"
    CLIENT_HOME[t]="$worker_home"
    CLIENT_LOGBASE[t]=0
  else
    cp "${CLIENT_PREFS[$t]}" "$WORKDIR/cli_$t/prefs.lua" || exit 2
  fi
  printf '%s\n' ${CLIENT_LIST[$t]//,/ } > "$WORKDIR/expected_$t.txt"
  printf '%s\t%s\n' "$t" "${CLIENT_ACCOUNT[t]}" >> "$WORKDIR/participant_accounts.tsv"
done
: > "$WORKDIR/owners.tsv"
for t in $(seq 1 "$PARTICIPANTS"); do
  for n in ${CLIENT_LIST[$t]//,/ }; do printf '%s\t%s\n' "$n" "${CLIENT_ACCOUNT[t]}"; done >> "$WORKDIR/owners.tsv"
done
printf '%s\n' "${NAMES[@]}" > "$WORKDIR/expected.txt"

say "starting state"
if [ "$API_ONLY" = 1 ]; then
  printf '  mode          : API only\n'
elif [ "$RESTART" = 1 ]; then
  cexec "systemctl stop $SERVICE >/dev/null 2>&1; pkill -9 -x $PROCNAME 2>/dev/null; for i in \$(seq 1 70); do grep -q ':1F90' /proc/net/tcp || break; sleep 1; done; systemctl start $SERVICE"
  for i in $(seq 1 60); do
    cexec "systemctl is-active $SERVICE" | grep -q '^active$' && break
    sleep 1
  done
  cexec "systemctl is-active $SERVICE" | grep -q '^active$' || die "service not active after restart"
  printf '  service       : restarted (%s)\n' "$SERVICE"
else
  cexec "pgrep -x $PROCNAME >/dev/null" || die "no '$PROCNAME' process in '$CONTAINER': start the server, or pass --force-server-start"
  printf '  service       : reusing the running server (pid %s)\n' "$(cexec "pgrep -x $PROCNAME | head -1" | tr -d ' ')"
fi
if [ "$LOCAL" = 1 ]; then
  if [ "$API_ONLY" = 0 ]; then BEFORE=$(wc -l < "$LOG") || die "cannot read local log: $LOG"; fi
  API_BEFORE=$(wc -l < "$APILOG") || die "cannot read local API log: $APILOG"
else
  if [ "$API_ONLY" = 0 ]; then BEFORE=$(cexec "wc -l < $LOG" | tr -d ' '); fi
  API_BEFORE=$(apec "wc -l < $APILOG" 2>/dev/null | tr -d ' ')
fi
printf '  api baseline  : line %s (%s:%s)\n' "${API_BEFORE:-0}" "$APIC" "$APILOG" 
if [ "$API_ONLY" = 0 ]; then
  printf '  log baseline  : line %s — every record after this line belongs to this run\n' "$BEFORE"

  KEY="_CONFIG_UFSRV_MEMBER_${FENCE_CLASS}_${FENCE_GEOGROUP}_${FENCE_INSTANCE}"
  STAMP=$(red hget "$KEY" last 2>/dev/null | tr -d '[:space:]')
  NOW=$(date +%s)
  printf '  instance key  : %s last=%s (now=%s)\n' "$KEY" "${STAMP:-none}" "$NOW"
fi

say "run"
run_participant() {
  t=$1
  args=(-P "$WORKDIR/cli_$t/prefs.lua")
  [ -z "$PROFILES_DIR" ] || args=(--home-base "${CLIENT_HOME[t]}")
  if [ "$API_ONLY" = 1 ]; then args+=(--api-only); fi
  cd "$WORKDIR/cli_$t" && ASAN_OPTIONS=detect_leaks=0 timeout "$TIMEOUT" "$CLI" --headless "${args[@]}" \
    --unfacd-group-join="${CLIENT_LIST[$t]}" >/dev/null 2>&1
  echo $? > "$WORKDIR/cli_$t/rc"
}
run_slot() {
  slot=$1
  for t in $(seq "$slot" "$WORKERS" "$PARTICIPANTS"); do ( run_participant "$t" ); done
}
for slot in $(seq 1 "$WORKERS"); do run_slot "$slot" & done
wait
RC=0
ACCEPTED=0
REFUSED=0
for t in $(seq 1 "$PARTICIPANTS"); do
  r=$(cat "$WORKDIR/cli_$t/rc" 2>/dev/null || echo 255)
  if [ "$r" = 0 ]; then ACCEPTED=$((ACCEPTED + 1)); else REFUSED=$((REFUSED + 1)); fi
  if [ "$MODE" = single ]; then
    [ "$r" = 0 ] || [ "$r" = 1 ] || [ "$RC" != 0 ] || RC=$r
  else
    [ "$r" = 0 ] || [ "$RC" != 0 ] || RC=$r
  fi
  if [ -n "$PROFILES_DIR" ]; then
    tail -n +"$((CLIENT_LOGBASE[t] + 1))" "${CLIENT_HOME[t]}/ufsrvcli.log" > "$WORKDIR/cli_$t/ufsrvcli.log" || die "cannot capture client log: ${CLIENT_HOME[t]}/ufsrvcli.log"
  fi
  cat "$WORKDIR/cli_$t/ufsrvcli.log" >> "$CLI_LOG" 2>/dev/null
done
if [ "$MODE" = single ] && { [ "$ACCEPTED" -ne 1 ] || [ "$REFUSED" -ne $((PARTICIPANTS - 1)) ]; }; then
  [ "$RC" != 0 ] || RC=1
fi
printf '  CLI exits     : %s (accepted=%s refused=%s)\n' "$(for t in $(seq 1 "$PARTICIPANTS"); do cat "$WORKDIR/cli_$t/rc" 2>/dev/null; done | tr '\n' ' ')" "$ACCEPTED" "$REFUSED"
[ -s "$CLI_LOG" ] || die "the CLI wrote no log under $WORKDIR/cli_*"

if [ "$API_ONLY" = 0 ]; then
  if [ "$LOCAL" = 1 ]; then
    tail -n +"$((BEFORE + 1))" "$LOG" > "$EVIDENCE" || die "cannot capture local log: $LOG"
  else
    cexec "tail -n +$((BEFORE + 1)) $LOG" > "$EVIDENCE"
  fi
  printf '  evidence      : %s (%s lines)\n' "$EVIDENCE" "$(wc -l < "$EVIDENCE" | tr -d ' ')"
fi
API_EVIDENCE="$(dirname "$EVIDENCE")/evidence_bulk_groups_api.log"
if [ -n "${API_BEFORE:-}" ]; then
  if [ "$LOCAL" = 1 ]; then
    tail -n +"$((API_BEFORE + 1))" "$APILOG" > "$API_EVIDENCE" || die "cannot capture local API log: $APILOG"
  else
    apec "tail -n +$((API_BEFORE + 1)) $APILOG" > "$API_EVIDENCE" 2>/dev/null
  fi
  printf '  api evidence  : %s (%s lines)\n' "$API_EVIDENCE" "$(wc -l < "$API_EVIDENCE" | tr -d ' ')"
else
  : > "$API_EVIDENCE"
fi

if [ "$API_ONLY" = 1 ]; then
  say "verdict"
  printf '  workers: requested=%s launched=%s (operations=%s)\n' "$REQUESTED_WORKERS" "$PARTICIPANTS" "$NGROUPS"
  if [ "$REQUESTED_WORKERS" -gt "$PARTICIPANTS" ]; then printf '  Worker count reduced to the number of operations.\n'; fi
  python3 "$(dirname "$0")/bulk_group_api_report.py" "$WORKDIR" "$PARTICIPANTS" "$API_EVIDENCE"
  exit "$?"
fi

say "corroboration"
CONFIRMED="$WORKDIR/confirmed.txt"
: > "$CONFIRMED"
python3 - "$WORKDIR" "$PARTICIPANTS" "$MODE" "$CONFIRMED" <<'PY'
import os, re, sys
workdir, clients, mode, out_path = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
conf, posted_total, honoured_all = [], 0, True
accepted = 0
for k in range(1, clients + 1):
    expected = [l.strip() for l in open(os.path.join(workdir, f"expected_{k}.txt")) if l.strip()]
    try:
        text = open(os.path.join(workdir, f"cli_{k}", "ufsrvcli.log"), errors="replace").read()
    except FileNotFoundError:
        honoured_all = False
        continue
    uids = set(re.findall(r"X-UFSRVUID: ([1-9][0-9]*)\s*$", text, re.MULTILINE))
    uid = next(iter(uids)) if len(uids) == 1 else "unknown"
    for fid, name in re.findall(r"group join: group\[\d+\] fid=(\d+) fname='([^']+)'", text):
        conf.append((fid, name, uid if name in expected else "unknown"))
    posted = re.findall(r"group join: POST \S+ \(fname=(\S+)\)", text)
    posted_expected = [n for n in posted if n in expected]
    posted_total += len(posted_expected)
    honoured = posted_expected == expected and bool(expected)
    if mode == "multi":
        honoured_all = honoured_all and honoured
    try:
        rc = int(open(os.path.join(workdir, f"cli_{k}", "rc")).read().strip())
    except (OSError, ValueError):
        rc = 255
    accepted += 1 if (rc == 0 and honoured) else 0
with open(out_path, "w") as f:
    for fid, name, uid in conf:
        f.write(f"{name}\t{fid}\t{uid}\n")
    f.write(f"#posted={posted_total}\n")
    f.write(f"#honoured={'true' if honoured_all else 'false'}\n")
    f.write(f"#accepted={accepted}\n")
PY
printf '  confirmed     : %s\n' "$(grep -vc '^#' "$CONFIRMED" | tr -d ' ')"
while IFS=$'\t' read -r name fid uid; do
  case "$name" in '#'*) continue ;; esac
  if [ "$(fencered exists "BID:$fid" 2>/dev/null | tr -d '[:space:]')" = "1" ]; then
    printf '  fence store   : %s -> fid %s present\n' "$name" "$fid"
  else
    printf '  fence store   : %s -> fid %s MISSING\n' "$name" "$fid"
  fi
done < "$CONFIRMED"

say "truth table"
python3 - "$EVIDENCE" "$CLI_LOG" "$WORKDIR/expected.txt" "$CATALOGUE" "$NGROUPS" "$RC" \
         "$STAMP" "$NOW" "$REDIS_FENCE" "$CONFIRMED" "$API_EVIDENCE" "$LOCAL" "$FENCE_PORT" "$REDIS_HOST" "$REDIS_SESSION" "$SESSION_PORT" \
         "$MODE" "$PARTICIPANTS" "$WORKDIR" <<'PY'
import math, re, subprocess, sys, unicodedata
from collections import Counter

(evidence, cli_log, expected_path, catalogue, groups, rc, stamp, now, redis_fence, confirmed_path,
 api_evidence, local, fence_port, redis_host, redis_session, session_port,
 mode, participant_count, workdir) = sys.argv[1:20]
local = (local == "1")
groups, rc, now, participant_count = int(groups), int(rc), int(now), int(participant_count)
expected = [l.strip() for l in open(expected_path) if l.strip()]
want = set(expected)
lines = open(evidence, errors='replace').read().splitlines()
text = "\n".join(lines)
cli = open(cli_log, errors='replace').read()

posted = re.findall(r"group join: POST \S+ \(fname=(\S+)\)", cli)
posted_expected = [n for n in posted if n in want]
confirmed = []
for l in open(confirmed_path):
    if l.startswith('#'):
        continue
    n, f, uid = l.rstrip("\n").split("\t")
    confirmed.append((n, f, uid))
confirmed_names = [n for n, _, _ in confirmed]

bc = re.compile(r"HandleIntraBroadcastForFence \{.*fname:'([^']+)'")
broadcasts = [m.group(1) for l in lines if (m := bc.search(l))]
broadcasts_expected = [n for n in broadcasts if n in want]
broadcasts_unexpected = [n for n in broadcasts if n not in want]

lookups = re.compile(r"FindFenceByCanonicalName .*COULD NOT FIND Fence '[^']*:([^']+)'")
lookup_names = []
for l in lines:
    if (m := lookups.search(l)):
        lookup_names.append(m.group(1).split(':')[-1])

lookup_names = [m.group(1).split(':')[-1] for l in lines if (m := lookups.search(l))]
per_name_lookup_ok = sum(1 for n in broadcasts_expected if n in lookup_names)

def in_store(fid):
    try:
        if local:
            cmd = ["redis-cli", "-h", redis_host, "-p", str(fence_port), "exists", f"BID:{fid}"]
        else:
            cmd = ["docker", "exec", redis_fence, "redis-cli", "exists", f"BID:{fid}"]
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        return out.stdout.strip() == "1"
    except Exception:
        return False

store_present = sum(1 for _, f, _ in confirmed if in_store(f))

def has_sole_member(name, fid, uid):
    if uid == "unknown":
        print(f"  fence membership: {name} fid={fid} could not identify submitting user")
        return False
    if local:
        cmd = ["redis-cli", "-h", redis_host, "-p", session_port]
    else:
        cmd = ["docker", "exec", redis_session, "redis-cli"]
    try:
        result = subprocess.run(cmd + ["--raw", "ZRANGE", f"MEMBER_USERS_FOR_FENCE:{fid}", "0", "1"],
                                capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"  fence membership: {name} fid={fid} Redis query failed: {exc}")
        return False
    members = result.stdout.splitlines()
    ok = result.returncode == 0 and members == [uid]
    print(f"  fence membership: {name} fid={fid} expected={[uid]} observed={members} "
          f"{'PASS' if ok else 'FAIL'}")
    return ok

name_counts = Counter(confirmed_names)
fid_counts = Counter(fid for _, fid, _ in confirmed)
sole_member_count = sum(
    has_sole_member(name, fid, uid)
    for name, fid, uid in confirmed
    if name in want and name_counts[name] == 1 and fid_counts[fid] == 1
)
canonical = {
    (name, fid): cname
    for fid, name, cname in re.findall(
        r"group join: group\[\d+\] fid=(\d+) fname='([^']+)' cname='([^']+)'", cli)
}

def redis_read(backend, port, *command):
    if local:
        cmd = ["redis-cli", "-h", redis_host, "-p", port]
    else:
        cmd = ["docker", "exec", backend, "redis-cli"]
    result = subprocess.run(cmd + ["--raw", *command], capture_output=True, text=True,
                            timeout=10, check=True)
    return result.stdout.splitlines()

def verify_bucket(check, name, fid, predicate):
    try:
        ok = bool(predicate())
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f"  {check} {name} fid={fid}: Redis check failed: {exc}")
        ok = False
    print(f"  {check} {name} fid={fid}: {'PASS' if ok else 'FAIL'}")
    return int(ok)

def score_is_zero(key, member):
    return redis_read(redis_fence, fence_port, "ZSCORE", key, member) == ["0"]

def creator_index_matches(fid, uid):
    forward = redis_read(redis_session, session_port, "ZSCORE", f"UF:{uid}", f"{fid}:0")
    reverse = redis_read(redis_session, session_port, "ZSCORE", f"MEMBER_USERS_FOR_FENCE:{fid}", uid)
    return forward == reverse and len(forward) == 1 and forward[0].isdigit() and int(forward[0]) > 0

def name_index_matches(cname, name, uid, fid, lng, lat):
    parts = cname.split(":", 4)
    if len(parts) != 5:
        return False
    components = [unicodedata.normalize("NFD", part.casefold()) for part in (*parts[:3], name) if part]
    if parts[3]:
        components.append(uid)
    return all(score_is_zero("FENCES_NAMEINDEX", f"{part}:{fid}:{cname}:{lng:.6f}:{lat:.6f}")
               for part in set(components))

def geo_matches(fid, lng, lat):
    coords = redis_read(redis_fence, fence_port, "GEOPOS", "FENCES_GEO", f"{fid}:2:")
    return len(coords) == 2 and all(
        math.isclose(float(actual), expected, abs_tol=0.00001, rel_tol=0)
        for actual, expected in zip(coords, (lng, lat))
    )

bucket_counts = dict.fromkeys(("C14", "C15", "C16", "C17", "C18"), 0)
for name, fid, uid in confirmed:
    if name not in want or name_counts[name] != 1 or fid_counts[fid] != 1 or uid == "unknown":
        continue
    cname = canonical.get((name, fid))
    try:
        record = redis_read(redis_fence, fence_port, "HMGET", f"BID:{fid}",
                            "id", "uid", "dname", "cname", "type", "lng", "lat", "event_counter")
        valid = (len(record) == 8 and record[:4] == [fid, uid, name, cname]
                 and int(record[4]) & (1 << 7) and int(record[7]) >= 2
                 and all(math.isfinite(float(value)) for value in record[5:7])
                 and redis_read(redis_fence, fence_port, "HSTRLEN", f"BID:{fid}", "fence_key") == ["32"])
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f"  C14 {name} fid={fid}: Redis check failed: {exc}")
        valid = False
    bucket_counts["C14"] += verify_bucket("C14", name, fid, lambda: valid)
    bucket_counts["C15"] += verify_bucket("C15", name, fid, lambda: cname and score_is_zero(
        "FENCES_MAIN", f"{cname}:{fid}"))
    if valid:
        lng, lat = map(float, record[5:7])
        bucket_counts["C16"] += verify_bucket("C16", name, fid, lambda: name_index_matches(
            cname, name, uid, fid, lng, lat))
        bucket_counts["C17"] += verify_bucket("C17", name, fid, lambda: geo_matches(fid, lng, lat))
    bucket_counts["C18"] += verify_bucket("C18", name, fid, lambda: creator_index_matches(fid, uid))

api_text = open(api_evidence, errors='replace').read() if api_evidence else ""
routing_failures = api_text.count("FOUND UNRESPONSIVE SERVER IN GEOGROUP")
fence_errors = len(re.findall(r"INVALID FENCE KEY|ERROR: COULD NOT (?:CREATE|JOIN) FENCE|FENCE COMMAND.*ERROR", text))
posted_count = len(posted_expected)
codes, honoured_participants = [], 0
for t in range(1, participant_count + 1):
    try:
        code = int(open(f"{workdir}/cli_{t}/rc").read().strip())
    except (OSError, ValueError):
        code = 255
    codes.append(code)
    try:
        expected_t = {l.strip() for l in open(f"{workdir}/expected_{t}.txt") if l.strip()}
        text_t = open(f"{workdir}/cli_{t}/ufsrvcli.log", errors="replace").read()
    except OSError:
        continue
    posted_t = [n for n in re.findall(r"group join: POST \S+ \(fname=(\S+)\)", text_t) if n in expected_t]
    if code == 0 and posted_t and set(posted_t) == expected_t:
        honoured_participants += 1
accepted = sum(1 for code in codes if code == 0)
refused = len(codes) - accepted
observed_c4 = (f"{accepted} accepted, {refused} refused" if mode == "single"
               else ("0" if accepted == len(codes) else "/".join(str(code) for code in codes)))
owners, participant_accounts, account_uid = {}, {}, {}
try:
    for line in open(f"{workdir}/owners.tsv"):
        owner_name, owner_account = line.rstrip("\n").split("\t")
        owners[owner_name] = owner_account
except OSError:
    pass
try:
    for line in open(f"{workdir}/participant_accounts.tsv"):
        participant, account = line.rstrip("\n").split("\t")
        participant_accounts[int(participant)] = account
except OSError:
    pass
for participant, account in participant_accounts.items():
    try:
        text_t = open(f"{workdir}/cli_{participant}/ufsrvcli.log", errors="replace").read()
    except OSError:
        continue
    uids = re.findall(r"X-UFSRVUID: ([1-9][0-9]*)\s*$", text_t, re.MULTILINE)
    if uids:
        account_uid[account] = uids[0]
attributed = sum(1 for name, fid, uid in confirmed
                 if not owners.get(name) or uid == account_uid.get(owners[name], uid))
duplicates = sum(count - 1 for count in name_counts.values() if count > 1)
fresh = False
try:
    fresh = int(stamp) > 0 and 0 <= (now - int(stamp)) < 300
except Exception:
    fresh = False

observed = {
 "C1": "equal" if posted_count == groups else f"{posted_count}/{groups}",
 "C2": len(confirmed_names),
 "C3": sum(1 for n in broadcasts if n not in want) + max(0, len(confirmed_names) - len(want)),
 "C4": observed_c4,
 "C5": len(broadcasts_expected),
 "C6": len(broadcasts_unexpected),
 "C7": per_name_lookup_ok,
 "C8": store_present,
 "C9": "true" if routing_failures <= groups else "false",
 "C10": fence_errors,
 "C11": "true" if fresh else "false",
 "C12": honoured_participants,
 "C13": sole_member_count,
 **bucket_counts,
 "C19": attributed,
 "C20": duplicates,
}

print(f"{'id':6} {'invariant':56} {'expected':22} {'observed':22} verdict")
print("-" * 120)
bad = 0
for ln in open(catalogue):
    if not ln.strip():
        continue
    pid, what, expect = ln.rstrip("\n").split("\t")
    if expect == "N":
        expect = str(groups)
    obs = observed.get(pid, "n/a")
    ok = str(obs) == expect
    print(f"{pid:6} {what:56} {expect:22} {str(obs):22} {'PASS' if ok else 'FAIL'}")
    bad += 0 if ok else 1
print("-" * 96)
print(f"groups={groups} posted={posted_count} confirmed={len(confirmed_names)} broadcasts={len(broadcasts_expected)} store={store_present}")
sys.exit(1 if bad else 0)
PY
TRUTH=$?

say "verdict"
if [ "$MODE" = single ]; then
  printf '  sign-on attempts: %s concurrent on one cookie (account %s)\n' "$PARTICIPANTS" "${CLIENTS[0]}"
else
  printf '  clients: %s independent accounts across %s workers (operations=%s)\n' "$PARTICIPANTS" "$WORKERS" "$NGROUPS"
fi
if [ "$REQUESTED_WORKERS" -gt "$WORKERS" ]; then
  printf '  Worker count reduced to the number of operations.\n'
fi
if [ "$TRUTH" = 0 ] && [ "$RC" = 0 ]; then
  if [ "$MODE" = single ]; then
    printf '  PASS — one sign-on accepted and the rest refused, %s groups created once by that account\n' "$NGROUPS"
  else
    printf '  PASS — %s groups created across %s independent workers, corroborated across CLI, websock syslog and the fence store\n' "$NGROUPS" "$PARTICIPANTS"
  fi
  exit 0
fi
printf '  FAIL — see the table above, %s and %s\n' "$EVIDENCE" "$CLI_LOG"
exit 1
