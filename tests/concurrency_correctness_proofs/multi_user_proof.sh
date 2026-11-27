#!/usr/bin/env bash
#
# Multi-user concurrency correctness proof — Appendix B of
# dev/technical_designs/UFSRV_SERVICE_REQUEST_CONCURRENCY_DESIGN.md
#
# N independent accounts, each with its own cookie/cid, driven concurrently and killed
# mid-flight. Establishes a fixed input context and a fixed run window on the running
# server (or on a freshly restarted one with --force-server-start), runs the scenario,
# then evaluates the
# invariant catalogue (Appendix A's set plus the isolation ones) against the run's log
# records and compares each result with the expected value. Exits non-zero on any
# violation.
#
# Usage:  ./multi_user_proof.sh [options]
#         ./multi_user_proof.sh --print-invariants      # pre-run context + catalogue
#
# Every path is absolute: a flag overrides its env var, which overrides the default.
#
# Env: CONTAINER SERVICE PROCNAME CLI PREFS_DIR ACCOUNTS ITER LOG EVIDENCE_DIR EVIDENCE SEED
#
#   CLI            ufsrvcli binary        (default /home/devops/repos/1.ufsrv/ufsrvcli/build/dev/src/ufsrvcli)
#   PREFS_DIR      holds u1.prefs .. uN.prefs  (default /tmp/mu)
#   EVIDENCE_DIR   evidence directory     (default /tmp/concurrency_correctness_proofs)
#   LOG            server log             (default /var/log/ufsrvwebsock.log)

set -uo pipefail

CONTAINER="${CONTAINER:-ufsrvwebsock}"
SERVICE="${SERVICE:-ufsrvwebsock@1}"
PROCNAME="${PROCNAME:-ufsrvwebsock}"
CLI="${CLI:-/home/devops/repos/1.ufsrv/ufsrvcli/build/dev/src/ufsrvcli}"
PREFS_DIR="${PREFS_DIR:-/tmp/mu}"
PROFILES_DIR="${PROFILES_DIR:-}"
ACCOUNTS="${ACCOUNTS:-3}"
WORKERS="${WORKERS:-1}"
ITER="${ITER:-15}"
LOG="${LOG:-/var/log/ufsrvwebsock.log}"
EVIDENCE_DIR="${EVIDENCE_DIR:-/tmp/concurrency_correctness_proofs}"
RESTART=0
LOCAL="${LOCAL:-0}"
PRINT_INVARIANTS=0
SEED="${SEED:-20261108}"

usage() {
  cat <<EOF
Usage: $0 [options]        (every path is absolute; flags override env override default)

  --cli PATH            ufsrvcli binary          (default $CLI)
  --prefs-dir DIR       holds u1.prefs .. uN.prefs (default $PREFS_DIR)
  --home-base DIR       root of numbered home bases holding <k>/ufsrvcli.prefs, k=1..accounts
                        (overrides --prefs-dir; the CLI is run with --home-base <DIR>/<k>)
  --evidence PATH       captured log window      (default $EVIDENCE_DIR/evidence_multi.log)
  --log PATH            server log in container  (default $LOG)
  --container NAME      container                (default $CONTAINER)
  --accounts N          participating accounts     (default $ACCOUNTS)
  --workers N           concurrent operations, independent of account count (default $WORKERS)
  --iter N              iterations per account   (default $ITER)
  --seed N              kill-delay seed          (default $SEED)
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
    --prefs-dir)        PREFS_DIR="$2"; shift 2 ;;
    --home-base)        PROFILES_DIR="$2"; shift 2 ;;
    --evidence)         EVIDENCE="$2"; shift 2 ;;
    --log)              LOG="$2"; shift 2 ;;
    --container)        CONTAINER="$2"; shift 2 ;;
    --accounts)         ACCOUNTS="$2"; shift 2 ;;
    --workers)          WORKERS="$2"; shift 2 ;;
    --iter)             ITER="$2"; shift 2 ;;
    --seed)             SEED="$2"; shift 2 ;;
    --print-invariants) PRINT_INVARIANTS=1; shift ;;
    --force-server-start) RESTART=1; shift ;;
    --local)            LOCAL=1; shift ;;
    -h|--help)          usage; exit 0 ;;
    *)                  printf 'unknown option: %s\n' "$1" >&2; usage; exit 2 ;;
  esac
done

for value in "$ACCOUNTS" "$ITER" "$WORKERS"; do
  case "$value" in ''|*[!0-9]*) printf 'FAIL(preflight): accounts, iter and workers must be positive integers\n' >&2; exit 2 ;; esac
  [ "$value" -gt 0 ] || { printf 'FAIL(preflight): accounts, iter and workers must be positive integers\n' >&2; exit 2; }
done
C=$((ACCOUNTS * ITER))
REQUESTED_WORKERS=$WORKERS
[ "$WORKERS" -le "$C" ] || WORKERS=$C
EVIDENCE="${EVIDENCE:-$EVIDENCE_DIR/evidence_multi.log}"
for v in "$CLI" "$PREFS_DIR" "$EVIDENCE" ${PROFILES_DIR:+"$PROFILES_DIR"}; do
  [ "${v#/}" = "$v" ] && { printf 'FAIL(preflight): path must be absolute: %s\n' "$v" >&2; exit 2; }
done
mkdir -p "$(dirname "$EVIDENCE")" || exit 2
CATALOGUE="$(dirname "$EVIDENCE")/invariants_multi.tsv"

fail=0
say() { printf '\n== %s\n' "$*"; }
die() { printf 'FAIL(preflight): %s\n' "$*" >&2; exit 2; }
if [ "$LOCAL" = 1 ] && [ -z "$PROFILES_DIR" ]; then
  die "--local requires --home-base DIR with client configurations"
fi
cexec() { docker exec "$CONTAINER" sh -c "$1"; }
if [ "$LOCAL" = 1 ]; then
  cexec() { sh -c "$1"; }
fi

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
P8	acquire failures (COULD NOT acquire TRY-WRITE/READ)	0
P9	stranded-session records (IOERROR / SESSION IN LOCK)	0
P10a	instate_success == wrapper_success	true
P10b	failure exits in the window	0
P11a	acquires >= connections (C)	true
P11b	releases <= acquires	true
R1	recycler start: first event is a dequeue with leased_sz 1	0
R2	recycler leased_sz arithmetic mismatches	0
R3	recycler ring progression violations (head/tail advance)	0
R4	double-lease refusals (ITEM NOT DEQUEUED)	0
R5	objects with an ambiguous rid mapping	0
R6	object enqueued twice without an intervening dequeue	0
P13	distinct sessions >= accounts	true
P14	cross-session contention (acquire failures)	0
P15a	sessions named by a migration exit but absent from the ledger	0
P15b	sessions left unbalanced	0
EOF
}

print_context() {
  printf '\n-- pre-run context (multi-user)\n'
  printf '  %-16s %s\n' container "$CONTAINER"
  printf '  %-16s %s\n' service "$SERVICE"
  printf '  %-16s %s\n' cli "$CLI"
  printf '  %-16s %s\n' 'prefs source' "$([ -n "$PROFILES_DIR" ] && echo "$PROFILES_DIR/<k>/ufsrvcli.prefs" || echo "$PREFS_DIR/u<k>.prefs")"
  printf '  %-16s %s\n' log "$LOG"
  printf '  %-16s %s\n' evidence "$EVIDENCE"
  printf '  %-16s %s x %s = %s connections\n' 'accounts x iter' "$ACCOUNTS" "$ITER" "$C"
  printf '  %-16s %s\n' seed "$SEED"
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

# ── 1. fixed input context: one registered prefs per account, cookies distinct ─
say "input context"
[ -x "$CLI" ] || die "CLI not executable: $CLI"
if [ "$LOCAL" = 1 ]; then
  [ "$RESTART" = 0 ] || die "--force-server-start is not available with --local (start the servers yourself)"
else
  docker inspect "$CONTAINER" >/dev/null 2>&1 || die "no container '$CONTAINER'"
fi
if [ "$LOCAL" = 1 ]; then
  [ -r "$LOG" ] || die "local log is not readable: $LOG"
else
  cexec "test -f $LOG" || die "no log at $LOG in $CONTAINER"
fi
declare -a COOKIES=()
for k in $(seq 1 "$ACCOUNTS"); do
  if [ -n "$PROFILES_DIR" ]; then
    p="$PROFILES_DIR/$k/ufsrvcli.prefs"
    if [ "$LOCAL" = 1 ]; then
      [ -r "$PROFILES_DIR/$k/ufsrvcli.config" ] || die "client configuration is not readable: $PROFILES_DIR/$k/ufsrvcli.config"
    fi
  else
    p="$PREFS_DIR/u$k.prefs"
  fi
  [ -f "$p" ] || die "missing prefs: $p (register $ACCOUNTS accounts: --home-base DIR, or --prefs-dir DIR)"
  c=$(sed -n 's/.*cookie *= *"\([^"]*\)".*/\1/p' "$p" | head -1)
  [ -n "$c" ] || die "no session.cookie in $p"
  COOKIES+=("$c")
done
dups=$(printf '%s\n' "${COOKIES[@]}" | sort | uniq -d | wc -l | tr -d ' ')
[ "$dups" = 0 ] || die "accounts must not share a cookie ($dups duplicate(s))"
if [ -n "$PROFILES_DIR" ]; then
  printf '  accounts      : %s (%s/1..%s/ufsrvcli.prefs)\n' "$ACCOUNTS" "$PROFILES_DIR" "$ACCOUNTS"
else
  printf '  accounts      : %s (%s/u1..u%s.prefs)\n' "$ACCOUNTS" "$PREFS_DIR" "$ACCOUNTS"
fi
printf '  cookies       : %s distinct\n' "$(printf '%s\n' "${COOKIES[@]}" | sort -u | wc -l | tr -d ' ')"
printf '  workers       : %s concurrent, %s total connections\n' "$WORKERS" "$C"

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

# ── 3. run: deterministic kill points ────────────────────────────────────────
say "run"
mapfile -t DELAYS < <(awk -v n="$C" -v s="$SEED" 'BEGIN{srand(s); for(i=0;i<n;i++) printf "%.2f\n", 0.10+rand()*1.90}')
RUN_HOME=$(mktemp -d "$EVIDENCE_DIR/multi_workers_XXXXXX") || exit 2
chmod 700 "$RUN_HOME"
worker() {
  local worker=$1 operation account d home
  for ((operation=worker; operation<C; operation+=WORKERS)); do
    account=$((operation % ACCOUNTS + 1))
    d=${DELAYS[$operation]}
    home="$RUN_HOME/$worker/$account"
    mkdir -p -m 700 "$home" || return 2
    if [ -n "$PROFILES_DIR" ]; then
      cp "$PROFILES_DIR/$account/ufsrvcli.config" "$PROFILES_DIR/$account/ufsrvcli.prefs" "$home/" || return 2
      chmod 600 "$home/ufsrvcli.config" "$home/ufsrvcli.prefs"
      timeout -s KILL "$d" "$CLI" --headless --home-base "$home" >/dev/null 2>&1
    else
      cp "$PREFS_DIR/u$account.prefs" "$home/prefs.lua" || return 2
      timeout -s KILL "$d" "$CLI" --headless -P "$home/prefs.lua" >/dev/null 2>&1
    fi
  done
}
for k in $(seq 0 $((WORKERS - 1))); do worker "$k" & done
wait
wait_quiescent
printf '  %s connections issued across %s accounts (SIGKILL 0.10-2.00s, seed %s)\n' "$C" "$ACCOUNTS" "$SEED"

# ── 4. capture the window ────────────────────────────────────────────────────
if [ "$LOCAL" = 1 ]; then
  tail -n +"$((BEFORE + 1))" "$LOG" > "$EVIDENCE" || die "cannot capture local log: $LOG"
else
  cexec "tail -n +$((BEFORE + 1)) $LOG" > "$EVIDENCE" || die "cannot capture server log: $LOG"
fi
printf '  evidence      : %s (%s lines)\n' "$EVIDENCE" "$(wc -l < "$EVIDENCE" | tr -d ' ')"

# ── 5. post-run checks against the catalogue ─────────────────────────────────
say "truth table"
python3 - "$EVIDENCE" "$C" "$ACCOUNTS" "$CATALOGUE" <<'PY'
import re, sys, collections

path, C, ACCOUNTS, catalogue = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
lines = open(path, errors='replace').read().splitlines()
text = "\n".join(lines)

A = re.compile(r"LOCKLEDGER A cid:'(\d+)' o:'(0x[0-9a-f]+)' tid:'(\d+)' ht:'(0x[0-9a-f]+)' held:(\d) try:(\d) func:'([^']*)'")
R = re.compile(r"LOCKLEDGER R cid:'(\d+)' o:'(0x[0-9a-f]+)' tid:'(\d+)' ht:'(0x[0-9a-f]+)' held:(\d) func:'([^']*)'")
seq = []
for l in lines:
    if (m := A.search(l)): seq.append(('A', int(m.group(5)), m.group(3), m.group(4), m.group(2), m.group(1)))
    elif (m := R.search(l)): seq.append(('R', int(m.group(5)), m.group(3), m.group(4), m.group(2), m.group(1)))
acq = [s for s in seq if s[0] == 'A']
rel = [s for s in seq if s[0] == 'R']

bal_bad = 0
state = collections.Counter()
for k, held, tid, ht, o, cid in seq:
    key = (tid, ht, o)
    if k == 'A' and held == 0: state[key] += 1
    elif k == 'R' and held == 1:
        state[key] -= 1
        if state[key] < 0: bal_bad += 1

mig_in  = len(re.findall(r"exit='instate_success'", text))
mig_out = len(re.findall(r"exit='wrapper_success'", text))
migrated_objs = set()
for l in lines:
    if "exit='instate_success'" in l:
        migrated_objs.update(re.findall(r"o_(?:transient|migrated):'(0x[0-9a-f]+)'", l))


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

observed = {
 "P1":  text.count("LOCK NOT OWNED BY THIS THREAD"),
 "P2":  text.count("COULD NOT RECORD LOCK OWNERSHIP"),
 "P3":  sum(1 for s in rel if s[1] == 0),
 "P4a": bal_bad,
 "P4b": sum(1 for v in state.values() if v > 0),
 "P6":  len({s[2] for s in seq}) - len({(s[2], s[3]) for s in seq}),
 "P8":  text.count("COULD NOT acquire TRY-WRITE/READ lock for Session events"),
 "P9":  text.count("LOCKEDSESNSTATUS_IOERROR") + len(re.findall(r"SESSION cid:'\d+' IN LOCK", text)),
 "P10a": mig_in == mig_out and mig_in > 0,
 "P10b": len(re.findall(r"exit='(?:instate|wrapper)_(?!success)[a-z_]+'", text)),
 "P11a": len(acq) >= C,
 "P11b": len(rel) <= len(acq),
 "R1":  r1_bad,
 "R2":  r2_bad,
 "R3":  r3_bad,
 "R4":  r4_bad,
 "R5":  r5_bad,
 "R6":  r6_bad,
 "P13": len({s[5] for s in seq}) >= ACCOUNTS,
 "P14": text.count("COULD NOT acquire TRY-WRITE/READ lock for Session events"),
 "P15a": len(migrated_objs - {s[4] for s in seq}),
 "P15b": sum(1 for v in state.values() if v != 0),
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
print(f"connections={C} accounts={ACCOUNTS} distinct_cids={len({s[5] for s in seq})} acquires={len(acq)} releases={len(rel)} migrations={mig_in}")
sys.exit(1 if bad else 0)
PY
[ $? -ne 0 ] && fail=1

# ── 6. verdict ───────────────────────────────────────────────────────────────
say "verdict"
printf '  workers: requested=%s launched=%s (operations=%s)\n' "$REQUESTED_WORKERS" "$WORKERS" "$C"
if [ "$REQUESTED_WORKERS" -gt "$WORKERS" ]; then
  printf '  Worker count reduced to the number of operations.\n'
fi
if [ "$fail" = 0 ]; then
  printf '  PASS — every invariant holds with its expected value over %s connections across %s accounts\n' "$C" "$ACCOUNTS"
  exit 0
fi
printf '  FAIL — see the table above and %s\n' "$EVIDENCE"
exit 1
