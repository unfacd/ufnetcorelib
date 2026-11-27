#!/usr/bin/env bash
#
# Rogue-client correctness proof — Appendix A of
# dev/technical_designs/UFSRV_SERVICE_REQUEST_CONCURRENCY_DESIGN.md
#
# One account, several processes sharing one prefs file (same cookie/cid), each killed
# mid-flight. Establishes a fixed input context and a fixed run window on the running
# server (or on a freshly restarted one with --force-server-start), runs the scenario,
# then evaluates the invariant catalogue against the run's log records and compares
# each result with the expected value. Exits non-zero if any invariant is violated.
#
# Usage:  ./rogue_client_proof.sh [options]
#         ./rogue_client_proof.sh --print-invariants      # pre-run context + catalogue
#
# Every path is absolute: a flag overrides its env var, which overrides the default.
#
# Env: CONTAINER SERVICE PROCNAME CLI PREFS WORKERS ITER LOG EVIDENCE_DIR EVIDENCE SEED
#
#   CLI            ufsrvcli binary        (default /home/devops/repos/1.ufsrv/ufsrvcli/build/dev/src/ufsrvcli)
#   PREFS          shared prefs file      (default /tmp/mu/rogue.prefs)
#   EVIDENCE_DIR   evidence directory     (default /tmp/concurrency_correctness_proofs)
#   LOG            server log             (default /var/log/ufsrvwebsock.log)

set -uo pipefail

for argument in "$@"; do
  if [ "$argument" = --same-cookie ]; then
    exec python3 "$(dirname -- "$0")/rogue_signon_proof.py" "$@"
  fi
done

CONTAINER="${CONTAINER:-ufsrvwebsock}"
SERVICE="${SERVICE:-ufsrvwebsock@1}"
PROCNAME="${PROCNAME:-ufsrvwebsock}"
CLI="${CLI:-/home/devops/repos/1.ufsrv/ufsrvcli/build/dev/src/ufsrvcli}"
PREFS="${PREFS:-/tmp/mu/rogue.prefs}"
PROFILES_DIR="${PROFILES_DIR:-}"
ACCOUNT="${ACCOUNT:-1}"
WORKERS="${WORKERS:-3}"
ITER="${ITER:-15}"
LOG="${LOG:-/var/log/ufsrvwebsock.log}"
EVIDENCE_DIR="${EVIDENCE_DIR:-/tmp/concurrency_correctness_proofs}"
RESTART=0
LOCAL="${LOCAL:-0}"
PRINT_INVARIANTS=0
SEED="${SEED:-20261108}"
ABORTS="${ABORTS:-8}"
WS_ADDR="${WS_ADDR:-}"
WS_PORT="${WS_PORT:-8080}"

usage() {
  cat <<EOF
Usage: $0 [options]        (every path is absolute; flags override env override default)

  --same-cookie         use synchronized sign-on election, live rejection and recovery proof
                        requires --home-base; use --same-cookie --help for its options
  --cli PATH            ufsrvcli binary          (default $CLI)
  --prefs PATH          shared prefs file        (default $PREFS)
  --home-base DIR       numbered home bases: use <DIR>/<account>/ufsrvcli.prefs and run the
                        CLI with --home-base <DIR>/<account> (overrides --prefs)
  --account K           home base number under --home-base (default $ACCOUNT)
  --evidence PATH       captured log window      (default $EVIDENCE_DIR/evidence_rogue.log)
  --log PATH            server log in container  (default $LOG)
  --container NAME      container                (default $CONTAINER)
  --workers N           concurrent processes     (default $WORKERS)
  --iter N              iterations per process   (default $ITER)
  --seed N              kill-delay seed          (default $SEED)
  --aborts N            rejected handshakes issued before the run (default $ABORTS; 0 disables)
  --ws-addr ADDR        container listener the aborts target (local: client config)
  --ws-port PORT        container listener port  (default $WS_PORT; local: client config)
  --print-invariants    print the pre-run context and the invariant catalogue, then run
  --force-server-start  restart the service before the run (default: use the running server)
  --local               run against servers on this host: no docker exec, log read locally
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
    --evidence)         EVIDENCE="$2"; shift 2 ;;
    --log)              LOG="$2"; shift 2 ;;
    --container)        CONTAINER="$2"; shift 2 ;;
    --workers)          WORKERS="$2"; shift 2 ;;
    --iter)             ITER="$2"; shift 2 ;;
    --seed)             SEED="$2"; shift 2 ;;
    --aborts)           ABORTS="$2"; shift 2 ;;
    --ws-addr)          WS_ADDR="$2"; shift 2 ;;
    --ws-port)          WS_PORT="$2"; shift 2 ;;
    --print-invariants) PRINT_INVARIANTS=1; shift ;;
    --force-server-start) RESTART=1; shift ;;
    --local)            LOCAL=1; shift ;;
    -h|--help)          usage; exit 0 ;;
    *)                  printf 'unknown option: %s\n' "$1" >&2; usage; exit 2 ;;
  esac
done

for value in "$WORKERS" "$ITER"; do
  case "$value" in ''|*[!0-9]*) printf 'FAIL(preflight): workers and iter must be positive integers\n' >&2; exit 2 ;; esac
  [ "$value" -gt 0 ] || { printf 'FAIL(preflight): workers and iter must be positive integers\n' >&2; exit 2; }
done
C=$((WORKERS * ITER))
EVIDENCE="${EVIDENCE:-$EVIDENCE_DIR/evidence_rogue.log}"
[ -n "$PROFILES_DIR" ] && PREFS="$PROFILES_DIR/$ACCOUNT/ufsrvcli.prefs"
for v in "$CLI" "$PREFS" "$EVIDENCE" ${PROFILES_DIR:+"$PROFILES_DIR"}; do
  [ "${v#/}" = "$v" ] && { printf 'FAIL(preflight): path must be absolute: %s\n' "$v" >&2; exit 2; }
done
mkdir -p "$(dirname "$EVIDENCE")" || exit 2
CATALOGUE="$(dirname "$EVIDENCE")/invariants_rogue.tsv"

fail=0
say() { printf '\n== %s\n' "$*"; }
die() { printf 'FAIL(preflight): %s\n' "$*" >&2; exit 2; }
if [ "$LOCAL" = 1 ] && [ -z "$PROFILES_DIR" ]; then
  die "--local requires --home-base DIR with client configurations"
fi
cexec() { docker exec "$CONTAINER" sh -c "$1"; }
cexec_bash() { docker exec "$CONTAINER" bash -c "$1"; }
if [ "$LOCAL" = 1 ]; then
  cexec() { sh -c "$1"; }
  cexec_bash() { bash -c "$1"; }
fi

fire_aborts() {
  local i
  for i in $(seq 1 "$ABORTS"); do
    cexec_bash "exec 3<>/dev/tcp/$WS_ADDR/$WS_PORT && printf 'GET / HTTP/1.1\r\nHost: $WS_ADDR\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n' >&3 && sleep 0.3; exec 3<&- 3>&-" || return 1
  done
}

wait_sweep() {
  local last=$1 i window
  for i in $(seq 1 40); do
    if [ "$LOCAL" = 1 ]; then
      window=$(tail -n +"$((last + 1))" "$LOG") || return 1
    else
      window=$(cexec "tail -n +$((last + 1)) $LOG") || return 1
    fi
    if printf '%s\n' "$window" | grep -q 'CheckSessionIdleTime'; then
      printf '  sweep         : first CheckSessionIdleTime after the aborts observed\n'
      return 0
    fi
    sleep 2
  done
  printf '  sweep         : TIMEOUT after 80s\n'
  return 1
}

# The server finishes tearing down a killed peer after the client is gone, so the capture
# must wait for the log to go quiet: P4b/P8/P14 are only meaningful on a quiescent tail.
wait_quiescent() {
  local last=-1 stable=0 n i
  for i in $(seq 1 60); do
    if [ "$LOCAL" = 1 ]; then
      n=$(wc -l < "$LOG") || return 1
    else
      n=$(cexec "wc -l < $LOG" | tr -d ' ') || return 1
    fi
    if [ "$n" = "$last" ]; then stable=$((stable + 1)); else stable=0; last=$n; fi
    if [ "$stable" -ge 3 ]; then
      printf '  quiescent     : no new records for 3s (line %s)\n' "$n"
      return 0
    fi
    sleep 1
  done
  printf '  quiescent     : TIMEOUT after 60s — capture may include teardown in progress\n'
  return 1
}

# ── invariant catalogue: the single source for the printout and for the check ──
write_catalogue() {
  cat > "$CATALOGUE" <<'EOF'
P1	releases refused (LOCK NOT OWNED)	0
P2	inserts refused (COULD NOT RECORD LOCK OWNERSHIP)	0
P3	releases with held:0 (release without ownership)	0
P4a	release observed before any acquire, per thread/object	0
P4b	objects still held at end of run	0
P6	threads whose records span more than one store	0
P8	acquire failures on an object never lockable again (wedge)	0
P9	stranded-session records (IOERROR / SESSION IN LOCK)	0
P10a	instate_success == wrapper_success	true
P10b	failure exits in the window	0
P11a	acquires >= connections (C)	true
P11b	releases <= acquires	true
P12	lost-reference holds left unreleased	0
P13	rejected handshakes observed >= injected aborts	true
R1	recycler start: first event is a dequeue with leased_sz 1	0
R2	recycler leased_sz arithmetic mismatches	0
R3	recycler ring progression violations (head/tail advance)	0
R4	double-lease refusals (ITEM NOT DEQUEUED)	0
R5	objects with an ambiguous rid mapping	0
R6	object enqueued twice without an intervening dequeue	0
EOF
}

print_context() {
  printf '\n-- pre-run context (rogue)\n'
  printf '  %-16s %s\n' container "$CONTAINER"
  printf '  %-16s %s\n' service "$SERVICE"
  printf '  %-16s %s\n' cli "$CLI"
  printf '  %-16s %s\n' prefs "$PREFS"
  printf '  %-16s %s\n' log "$LOG"
  printf '  %-16s %s\n' evidence "$EVIDENCE"
  printf '  %-16s %s x %s = %s connections\n' 'workers x iter' "$WORKERS" "$ITER" "$C"
  printf '  %-16s %s\n' seed "$SEED"
  printf '  %-16s %s\n' aborts "$ABORTS against ${WS_ADDR:-<container address>}:$WS_PORT"
  printf '  %-16s %s\n' 'server start' "$([ "$RESTART" = 1 ] && echo 'restart' || echo 'use running')"
  printf '\n-- invariant catalogue (%s)\n\n' "$CATALOGUE"
  printf '  %-6s %-56s %s\n' id invariant expected
  printf '  %s\n' "--------------------------------------------------------------------------------"
  while IFS=$'\t' read -r id what expect; do
    printf '  %-6s %-56s %s\n' "$id" "$what" "$expect"
  done < "$CATALOGUE"
}

write_catalogue
if [ "$PRINT_INVARIANTS" = 1 ]; then print_context; fi

# ── 1. fixed input context ───────────────────────────────────────────────────
say "input context"
[ -x "$CLI" ] || die "CLI not executable: $CLI"
if [ "$LOCAL" = 1 ]; then
  [ "$RESTART" = 0 ] || die "--force-server-start is not available with --local (start the servers yourself)"
else
  docker inspect "$CONTAINER" >/dev/null 2>&1 || die "no container '$CONTAINER'"
fi
if [ "$ABORTS" -gt 0 ]; then
  if [ "$LOCAL" = 1 ]; then
    endpoint=$(python3 - "$PROFILES_DIR/$ACCOUNT/ufsrvcli.config" <<'PY_ENDPOINT'
import ipaddress, re, sys
try:
    text = open(sys.argv[1]).read()
    text = re.sub(r"""("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')|--\[\[.*?\]\]|--[^\n]*""",
                  lambda m: m.group(1) or "", text, flags=re.S)
    backend = re.search(r'\bufsrvwebsock_backend\s*=\s*\{([^{}]*)\}', text)
    if not backend:
        raise ValueError("explicit ufsrvwebsock_backend required for local abort probes")
    address = re.search(r'\baddress\s*=\s*"([^"\\]+)"', backend[1])
    port = re.search(r'\bport\s*=\s*(\d+)\s*[,}]?', backend[1])
    if not address or not port:
        raise ValueError("explicit address and port required for local abort probes")
    if not ipaddress.ip_address(address[1]).is_loopback or not 1 <= int(port[1]) <= 65535:
        raise ValueError("local abort probes require a loopback address and valid port")
    if re.search(r'\btls\s*=\s*true\b', backend[1]):
        raise ValueError("raw abort probes require a plaintext listener; use --aborts 0 for TLS")
    print(address[1], int(port[1]))
except (OSError, ValueError) as exc:
    sys.exit(str(exc))
PY_ENDPOINT
) || die "cannot resolve abort listener from client configuration"
    read -r WS_ADDR WS_PORT <<< "$endpoint"
  elif [ -z "$WS_ADDR" ]; then
    WS_ADDR=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$CONTAINER" 2>/dev/null)
    [ -n "$WS_ADDR" ] || die "could not resolve the container address; pass --ws-addr"
  fi
fi
if [ "$LOCAL" = 1 ]; then
  [ -r "$LOG" ] || die "local log is not readable: $LOG"
else
  cexec "test -f $LOG" || die "no log at $LOG in $CONTAINER"
fi
[ -f "$PREFS" ] || die "no prefs file: $PREFS (register one and point --prefs at it, or use --home-base DIR)"
grep -q 'cookie' "$PREFS" || die "prefs has no session.cookie: $PREFS"
if [ "$LOCAL" = 1 ]; then
  [ -r "$PROFILES_DIR/$ACCOUNT/ufsrvcli.config" ] || die "client configuration is not readable: $PROFILES_DIR/$ACCOUNT/ufsrvcli.config"
fi
printf '  account prefs : %s\n' "$PREFS"
printf '  workers       : %s x %s = %s connections\n' "$WORKERS" "$ITER" "$C"
printf '  aborts        : %s upgrade requests the server rejects (%s:%s)\n' "$ABORTS" "${WS_ADDR:-none}" "$WS_PORT"

# ── 2. fixed run window: the running server, or a freshly restarted one ──────
say "starting state"
if [ "$RESTART" = 1 ]; then
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
  BEFORE=$(wc -l < "$LOG") || die "cannot read local log: $LOG"
else
  BEFORE=$(cexec "wc -l < $LOG" | tr -d ' ') || die "cannot read server log: $LOG"
fi
printf '  log baseline  : line %s — every record after this line belongs to this run\n' "$BEFORE"

if [ "$ABORTS" -gt 0 ]; then
  say "rejected handshakes"
  fire_aborts || die "abort probe could not open $WS_ADDR:$WS_PORT in $CONTAINER"
  if [ "$LOCAL" = 1 ]; then
    ABORTS_END=$(wc -l < "$LOG") || die "cannot read local log: $LOG"
  else
    ABORTS_END=$(cexec "wc -l < $LOG" | tr -d ' ')
  fi
  printf '  issued        : %s upgrade requests the server rejects\n' "$ABORTS"
  printf '  settling      : one sweep interval before the run\n'
  sleep 62
  wait_sweep "$ABORTS_END" || true
fi

# ── 3. run: deterministic kill points ────────────────────────────────────────
say "run"
mapfile -t DELAYS < <(awk -v n="$C" -v s="$SEED" 'BEGIN{srand(s); for(i=0;i<n;i++) printf "%.2f\n", 0.10+rand()*1.90}')
RUN_HOME=$(mktemp -d "$EVIDENCE_DIR/rogue_workers_XXXXXX") || exit 2
chmod 700 "$RUN_HOME"
worker() {
  local k=$1 i d home
  home="$RUN_HOME/$k/$ACCOUNT"
  mkdir -p -m 700 "$home" || return 2
  if [ -n "$PROFILES_DIR" ]; then
    cp "$PROFILES_DIR/$ACCOUNT/ufsrvcli.config" "$PROFILES_DIR/$ACCOUNT/ufsrvcli.prefs" "$home/" || return 2
    chmod 600 "$home/ufsrvcli.config" "$home/ufsrvcli.prefs"
  else
    cp "$PREFS" "$home/prefs.lua" || return 2
  fi
  for i in $(seq 0 $((ITER - 1))); do
    d=${DELAYS[$((k * ITER + i))]}
    if [ -n "$PROFILES_DIR" ]; then
      timeout -s KILL "$d" "$CLI" --headless --home-base "$home" >/dev/null 2>&1
    else
      timeout -s KILL "$d" "$CLI" --headless -P "$home/prefs.lua" >/dev/null 2>&1
    fi
  done
}
for k in $(seq 0 $((WORKERS - 1))); do worker "$k" & done
wait
wait_quiescent
printf '  %s connections issued (SIGKILL 0.10-2.00s, seed %s)\n' "$C" "$SEED"

# ── 4. capture the window ────────────────────────────────────────────────────
if [ "$LOCAL" = 1 ]; then
  tail -n +"$((BEFORE + 1))" "$LOG" > "$EVIDENCE" || die "cannot capture local log: $LOG"
else
  cexec "tail -n +$((BEFORE + 1)) $LOG" > "$EVIDENCE" || die "cannot capture server log: $LOG"
fi
printf '  evidence      : %s (%s lines)\n' "$EVIDENCE" "$(wc -l < "$EVIDENCE" | tr -d ' ')"

# ── 5. post-run checks against the catalogue ─────────────────────────────────
say "truth table"
python3 - "$EVIDENCE" "$C" "$CATALOGUE" "$ABORTS" <<'PY'
import re, sys, collections

path, C, catalogue, injected = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4])
lines = open(path, errors='replace').read().splitlines()
text = "\n".join(lines)

A = re.compile(r"LOCKLEDGER A cid:'(\d+)' o:'(0x[0-9a-f]+)' tid:'(\d+)' ht:'(0x[0-9a-f]+)' held:(\d) try:(\d) func:'([^']*)'")
R = re.compile(r"LOCKLEDGER R cid:'(\d+)' o:'(0x[0-9a-f]+)' tid:'(\d+)' ht:'(0x[0-9a-f]+)' held:(\d) func:'([^']*)'")
seq = []
for i, l in enumerate(lines):
    if (m := A.search(l)):
        nxt = " ".join(lines[i + 1:i + 4])
        seq.append(('A', int(m.group(5)), m.group(3), m.group(4), m.group(2), m.group(1),
                    "SUCCESS" in nxt and "lock for Session events acquired" in nxt))
    elif (m := R.search(l)):
        seq.append(('R', int(m.group(5)), m.group(3), m.group(4), m.group(2), m.group(1), True))
acq = [s for s in seq if s[0] == 'A' and s[6]]
rel = [s for s in seq if s[0] == 'R']

bal_bad = 0
state = collections.Counter()
for k, held, tid, ht, o, cid, ok in seq:
    key = (tid, ht, o)
    if k == 'A' and held == 0 and ok: state[key] += 1
    elif k == 'R' and held == 1:
        state[key] -= 1
        if state[key] < 0: bal_bad += 1
mig_in  = len(re.findall(r"exit='instate_success'", text))
mig_out = len(re.findall(r"exit='wrapper_success'", text))


# ── recycler lifecycle: sDeQueue / sEnQueue ring records ──────────────────────
RC = re.compile(r"s(De|En)Queue \{pid:'\d+', groupid:'(\d+)', tail_pos:'(\d+)', head_pos:'(\d+)', o:'(0x[0-9a-f]+)', rid:'(\d+)', type_name:'([^']*)', leased_sz:'(\d+)'")
rc = []                                   # (kind, groupid, tail, head, obj, rid, type, leased, line_index)
for i, l in enumerate(lines):
    if (m := RC.search(l)):
        rc.append(('D' if m.group(1) == 'De' else 'E', m.group(2), int(m.group(3)), int(m.group(4)),
                   m.group(5), m.group(6), m.group(7), int(m.group(8)), i))

# R1: first recycler record per group is a dequeue showing leased_sz 1
first_by_group = {}
for e in rc: first_by_group.setdefault(e[1], e)
r1_bad = sum(1 for e in first_by_group.values() if not (e[0] == 'D' and e[7] == 1))
# R2: the logged leased_sz must equal the model after each event
model = collections.defaultdict(int)
r2_bad = 0
for k, g, tail, head, o, rid, tname, leased, _ in rc:
    model[g] += 1 if k == 'D' else -1
    if model[g] != leased or model[g] < 0: r2_bad += 1
# R3: consecutive dequeues advance head by one; consecutive enqueues advance tail by one
r3_bad = 0
prev = {}
for k, g, tail, head, o, rid, tname, leased, _ in rc:
    pos = head if k == 'D' else tail
    key = (g, k)
    if key in prev:
        p = prev[key]
        if not (pos == p + 1 or (pos == 0 and p > 0)): r3_bad += 1
    prev[key] = pos
# R4: double-lease refusals
r4_bad = text.count("ITEM NOT DEQUEUED")
# R5: rid <-> payload pointer must be a bijection within a group/type
r5_bad = 0
for g in {e[1] for e in rc}:
    by_rid, by_obj = collections.defaultdict(set), collections.defaultdict(set)
    for k, gg, tail, head, o, rid, tname, leased, _ in rc:
        if gg != g: continue
        by_rid[rid].add(o); by_obj[o].add(rid)
    r5_bad += sum(1 for v in by_rid.values() if len(v) > 1) + sum(1 for v in by_obj.values() if len(v) > 1)
# R6: an object may not be enqueued twice without an intervening dequeue
r6_bad = 0
prevk = {}
for k, g, tail, head, o, rid, tname, leased, _ in rc:
    if prevk.get(o) == 'E' and k == 'E': r6_bad += 1
    prevk[o] = k

lost_objs = [(i, m.group(1)) for i, l in enumerate(lines)
             if "WE LOST REFERENCE TO SESSION" in l and (m := re.search(r"o:'(0x[0-9a-f]+)'", l))]
lost_unreleased = 0
for i, o in lost_objs:
    window = lines[i + 1:i + 6]
    if not any(("SessionUnLock" in w) and ("o:'%s'" % o in w) and ("SUCCESS" in w) for w in window):
        lost_unreleased += 1
rejected = sum(1 for l in lines if "COULD NOT HANDSHAKE" in l)
FAILED = re.compile(r"SessionLockRWCtx .*o:'(0x[0-9a-f]+)'.*COULD NOT acquire")
LOCKED = re.compile(r"SessionLockRWCtx .*o:'(0x[0-9a-f]+)'.*lock for Session events acquired")
last_ok = {}
for i, l in enumerate(lines):
    if (m := LOCKED.search(l)): last_ok[m.group(1)] = i
run_fail = [(i, m.group(1)) for i, l in enumerate(lines) if (m := FAILED.search(l))]
wedge_fail = sum(1 for i, o in run_fail if last_ok.get(o, -1) < i)

observed = {
 "P1":  text.count("LOCK NOT OWNED BY THIS THREAD"),
 "P2":  text.count("COULD NOT RECORD LOCK OWNERSHIP"),
 "P3":  sum(1 for s in rel if s[1] == 0),
 "P4a": bal_bad,
 "P4b": sum(1 for v in state.values() if v > 0),
 "P6":  len({s[2] for s in seq}) - len({(s[2], s[3]) for s in seq}),
 "P8":  wedge_fail,
 "P8t": len(run_fail),
 "P9":  text.count("LOCKEDSESNSTATUS_IOERROR") + len(re.findall(r"SESSION cid:'\d+' IN LOCK", text)),
 "P10a": mig_in == mig_out and mig_in > 0,
 "P10b": len(re.findall(r"exit='(?:instate|wrapper)_(?!success)[a-z_]+'", text)),
 "P11a": len(acq) >= C,
 "P11b": len(rel) <= len(acq),
 "P12": lost_unreleased,
 "P13": rejected >= injected,
 "R1":  r1_bad,
 "R2":  r2_bad,
 "R3":  r3_bad,
 "R4":  r4_bad,
 "R5":  r5_bad,
 "R6":  r6_bad,
}
def norm(v):
    return "true" if v is True else "false" if v is False else str(v)

print(f"{'id':6} {'invariant':56} {'expected':10} {'observed':10} verdict")
print("-" * 96)
bad = 0
for ln in open(catalogue):
    if not ln.strip(): continue
    pid, what, expect = ln.rstrip("\n").split("\t")
    obs = norm(observed.get(pid, "n/a"))
    ok = (obs == expect)
    print(f"{pid:6} {what:56} {expect:10} {obs:10} {'PASS' if ok else 'FAIL'}")
    bad += 0 if ok else 1
print("-" * 96)
print(f"connections={C} acquires={len(acq)} releases={len(rel)} migrations={mig_in} rejected={rejected} lost={len(lost_objs)} acquire_failures={len(run_fail)} wedges={wedge_fail}")
sys.exit(1 if bad else 0)
PY
[ $? -ne 0 ] && fail=1

# ── 6. verdict ───────────────────────────────────────────────────────────────
say "verdict"
printf '  workers: requested=%s launched=%s (operations=%s, iter=%s per worker)\n' "$WORKERS" "$WORKERS" "$C" "$ITER"
if [ "$fail" = 0 ]; then
  printf '  PASS — every invariant holds with its expected value over %s connections\n' "$C"
  exit 0
fi
printf '  FAIL — see the table above and %s\n' "$EVIDENCE"
exit 1
