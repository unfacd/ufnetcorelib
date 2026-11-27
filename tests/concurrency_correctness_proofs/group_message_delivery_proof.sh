#!/usr/bin/env bash
exec python3 - "$0" "$@" <<'PY'
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import math
import os
from pathlib import Path
import re
import subprocess
import shutil
import sys
import tempfile
from threading import Lock
import time
import unicodedata

sys.path.insert(0, str(Path(sys.argv[1]).resolve().parent))
from message_proof_checks import intra_requests_processed

parser = argparse.ArgumentParser(prog=sys.argv[1], description="Prove shared fence creation and message processing.")
parser.add_argument("--cli", default=os.environ.get("CLI", "/home/devops/repos/1.ufsrv/ufsrvcli/build/dev/src/ufsrvcli"))
parser.add_argument("--home-base", type=Path, default=os.environ.get("HOME_BASE"))
parser.add_argument("--account", type=int, default=1, help="first participant account; independent of worker count")
parser.add_argument("--workers", type=int, default=os.environ.get("WORKERS", 1), help="maximum concurrent CLI operations")
parser.add_argument("--agents", type=int, default=os.environ.get("AGENTS", 10))
parser.add_argument("--messages", type=int, default=os.environ.get("MESSAGES", 10), help="messages per agent")
parser.add_argument("--prefix", default=os.environ.get("PREFIX", "msgproof"))
parser.add_argument("--local", action="store_true", default=os.environ.get("LOCAL") == "1")
parser.add_argument("--container", default=os.environ.get("CONTAINER", "ufsrvwebsock"))
parser.add_argument("--api-container", default=os.environ.get("APIC", "ufsrvapi"))
parser.add_argument("--log", type=Path, default=os.environ.get("LOG", "/var/log/ufsrvwebsock.log"))
parser.add_argument("--api-log", type=Path, default=os.environ.get("APILOG", "/var/log/ufsrvapi.log"))
parser.add_argument("--evidence", type=Path, default=os.environ.get("EVIDENCE"))
parser.add_argument("--redis-fence", default=os.environ.get("REDIS_FENCE", "redis-fence"))
parser.add_argument("--redis-session", default=os.environ.get("REDIS_SESSION", "redis-session"))
parser.add_argument("--redis-usrmsg", default=os.environ.get("REDIS_USRMSG", "redis-usrmsg"))
parser.add_argument("--fence-class", default=os.environ.get("FENCE_CLASS", "ufsrvwebsock"))
parser.add_argument("--geogroup", type=int, default=os.environ.get("FENCE_GEOGROUP", 3))
parser.add_argument("--join-timeout", type=float, default=os.environ.get("JOIN_TIMEOUT", 30))
parser.add_argument("--message-timeout", type=float, default=os.environ.get("MESSAGE_TIMEOUT", 20))
parser.add_argument("--launch-spacing", type=float, default=1.2, help="seconds between CLI starts on the shared source IP")
parser.add_argument("--print-invariants", action="store_true")
args = parser.parse_args(sys.argv[2:])
if args.local and args.home_base is None:
    parser.error("--local requires --home-base")
if args.account < 1 or args.workers < 1 or args.agents < 2 or args.messages < 1 or args.geogroup < 0:
    parser.error("requires positive account/workers, at least two agents, one message per agent and a nonnegative geogroup")
if any(not math.isfinite(value) or value <= 0 for value in (args.join_timeout, args.message_timeout)):
    parser.error("timeouts must be finite and positive")
if not math.isfinite(args.launch_spacing) or args.launch_spacing < 0:
    parser.error("launch spacing must be finite and nonnegative")
if not re.fullmatch(r"[A-Za-z0-9_-]{1,30}", args.prefix):
    parser.error("--prefix must contain 1–30 letters, digits, underscores or hyphens")
home_base = (args.home_base or Path.home() / "ufsrvcli/test_agents").resolve()
cli = Path(args.cli).resolve()
total = args.agents * args.messages
worker_count = min(args.workers, total)
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
host = os.environ.get("REDIS_HOST", "127.0.0.1")
redis_cli = os.environ.get("REDIS_CLI", "redis-cli")
ports = {args.redis_fence: os.environ.get("FENCE_PORT", "63792"),
         args.redis_session: os.environ.get("SESSION_PORT", "63791"),
         args.redis_usrmsg: os.environ.get("USRMSG_PORT", "63793")}
checks = []
phase = "preflight"
launch_lock = Lock()
next_launch = 0.0

def check(label, passed):
    checks.append(bool(passed))
    print(f"{'PASS' if passed else 'FAIL'} {label}", flush=True)

def require(label, passed):
    check(label, passed)
    if not passed:
        raise ValueError(label)

def run(command, timeout=15):
    return subprocess.check_output(command, text=True, errors="replace", timeout=timeout)

def redis(backend, *command):
    prefix = ([redis_cli, "-h", host, "-p", ports[backend]] if args.local
              else ["docker", "exec", backend, "redis-cli"])
    return run(prefix + ["--raw", *command]).rstrip("\n")

def log_lines(path, container):
    if args.local:
        return path.read_text(errors="replace").splitlines()
    return run(["docker", "exec", container, "cat", str(path)]).splitlines()

def window(path, container, before):
    after = log_lines(path, container)
    if len(after) < len(before) or (before and after[len(before) - 1] != before[-1]):
        raise ValueError(f"server log rotated during run: {path}")
    return "\n".join(after[len(before):]) + "\n"

def invoke(account, option, label, timeout):
    global next_launch
    log = workdir / f"agent_{account}_{label}.log"
    invocation = workdir / f'profile_{account}_{label}' / str(account)
    invocation.mkdir(parents=True, mode=0o700)
    invocation.parent.chmod(0o700)
    for filename in ('ufsrvcli.config', 'ufsrvcli.prefs'):
        shutil.copyfile(home_base / str(account) / filename, invocation / filename)
        (invocation / filename).chmod(0o600)
    command = [str(cli), "--headless", "--home-base", str(invocation), option, "--"]
    with log.open("w") as stream:
        with launch_lock:
            time.sleep(max(0, next_launch - time.monotonic()))
            process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT, env=env)
            next_launch = time.monotonic() + args.launch_spacing
        try:
            rc = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            rc = 124
    return rc, log.read_text(errors="replace")

def identity(text, header="X-UFSRVUID"):
    ids = set(re.findall(header + r": ([1-9][0-9]*)[ \t]*$", text, re.M))
    if len(ids) != 1:
        raise ValueError(f"missing or ambiguous {header} in CLI handshake")
    return ids.pop()

def score(backend, key, member):
    return redis(backend, "ZSCORE", key, member)

def event_counter(fid):
    return int(redis(args.redis_fence, "HGET", f"BID:{fid}", "event_counter"))

def check_fence(fid, cname, creator, uids):
    record = redis(args.redis_fence, "HMGET", f"BID:{fid}",
                   "id", "uid", "dname", "cname", "type", "lng", "lat").splitlines()
    require("fence hash matches confirmed identity and creator",
            len(record) == 7 and record[:4] == [fid, creator, name, cname] and int(record[4]) & (1 << 7))
    check("fence key has 32 bytes", redis(args.redis_fence, "HSTRLEN", f"BID:{fid}", "fence_key") == "32")
    lng, lat = map(float, record[5:])
    check("canonical registry has exactly one entry for the fence name",
          redis(args.redis_fence, "ZRANGEBYLEX", "FENCES_MAIN", f"[{cname}:", f"({cname};") == f"{cname}:{fid}"
          and score(args.redis_fence, "FENCES_MAIN", f"{cname}:{fid}") == "0")
    parts = cname.split(":", 4)
    if len(parts) != 5:
        raise ValueError(f"malformed canonical name: {cname}")
    components = {unicodedata.normalize("NFD", part.casefold()) for part in (*parts[:3], name) if part}
    if parts[3]:
        components.add(creator)
    check("all fence name and location entries exist", all(
        score(args.redis_fence, "FENCES_NAMEINDEX", f"{part}:{fid}:{cname}:{lng:.6f}:{lat:.6f}") == "0"
        for part in components))
    coords = redis(args.redis_fence, "GEOPOS", "FENCES_GEO", f"{fid}:2:").splitlines()
    check("geo index matches fence coordinates", len(coords) == 2 and all(
        math.isclose(float(actual), expected, abs_tol=0.00001, rel_tol=0)
        for actual, expected in zip(coords, (lng, lat))))
    members_key = f"MEMBER_USERS_FOR_FENCE:{fid}"
    require("reverse membership contains exactly the participating accounts",
            sorted(redis(args.redis_session, "ZRANGE", members_key, "0", "-1").splitlines()) == sorted(uids))
    for uid in uids:
        forward = score(args.redis_session, f"UF:{uid}", f"{fid}:0")
        require(f"both membership indexes agree for uid={uid}",
                forward.isdigit() and int(forward) > 0 and forward == score(args.redis_session, members_key, uid))

def send_message(operation):
    account, ordinal = operation
    tag = f"{name}_a{account}_m{ordinal}"
    return invoke(account, f"--unfacd-group-message={fid}:{tag}",
                  f"message_{ordinal}", args.message_timeout)

try:
    if not cli.is_file() or not os.access(cli, os.X_OK):
        raise ValueError(f"CLI is not executable: {cli}")
    accounts = list(range(args.account, args.account + args.agents))
    for account in accounts:
        for filename in ("ufsrvcli.config", "ufsrvcli.prefs"):
            path = home_base / str(account) / filename
            if not path.is_file() or not os.access(path, os.R_OK):
                raise ValueError(f"missing or unreadable profile file: {path}")
    for backend in ports:
        if redis(backend, "PING") != "PONG":
            raise ValueError(f"Redis unavailable: {backend}")
    api_before = log_lines(args.api_log, args.api_container)
    ws_before = log_lines(args.log, args.container)
    evidence_dir = Path(os.environ.get("EVIDENCE_DIR", "/tmp/concurrency_correctness_proofs"))
    evidence_dir.mkdir(parents=True, exist_ok=True)
    workdir = Path(tempfile.mkdtemp(prefix="messages_", dir=evidence_dir))
    name = f"{args.prefix}_{time.strftime('%Y%m%d%H%M%S')}_{workdir.name.split('_')[-1]}"
    print(f"Fence {name}, {args.agents} agents × {args.messages} messages. Evidence: {workdir}", flush=True)
    if args.print_invariants:
        print("Expected: one created fence, all agents joined, all fence and membership indexes consistent, "
              f"{total} API INTRA requests consumed, {total} message insert records and event increments, "
              "and paired outbound staging records wherever payloads remain queued.", flush=True)
    phase = "proof"
    with ThreadPoolExecutor(max_workers=min(worker_count, args.agents)) as workers:
        joins = list(workers.map(lambda account: invoke(
            account, f"--unfacd-group-join={name}", "join", args.join_timeout), accounts))
    for account, (rc, _) in zip(accounts, joins):
        require(f"agent {account} join exited 0 (exit={rc}, log=agent_{account}_join.log)", rc == 0)
    uids = [identity(text) for _, text in joins]
    cids = [identity(text, "X-UFSRVCID") for _, text in joins]
    require("agents authenticated as distinct accounts", len(set(uids)) == args.agents)
    confirmations = []
    creators = []
    for account, uid, (rc, text) in zip(accounts, uids, joins):
        matches = re.findall(r"group join: group\[\d+\] fid=(\d+) fname='([^']+)' cname='([^']+)'", text)
        require(f"agent {account} confirmed one join for the test fence", rc == 0 and
                len(matches) == 1 and matches[0][1] == name)
        confirmations.append((matches[0][0], matches[0][2]))
        if re.search(r"group join: outcome\[\d+\] CREATED\b", text):
            creators.append(uid)
    require("one created fence and one shared confirmed ID", len(creators) == 1 and len(set(confirmations)) == 1)
    fid, cname = confirmations[0]
    (workdir / "fence.tsv").write_text(f"{name}\t{fid}\t{cname}\t{creators[0]}\n")
    check_fence(fid, cname, creators[0], uids)
    before = event_counter(fid)
    check("creation and joins advanced the event counter", before == args.agents + 1)
    print(f"Sending {total} messages to fid={fid}", flush=True)
    operations = [(account, ordinal) for account in accounts for ordinal in range(1, args.messages + 1)]
    with ThreadPoolExecutor(max_workers=worker_count) as workers:
        flat_results = list(workers.map(send_message, operations))
    messages = [flat_results[offset:offset+args.messages] for offset in range(0, total, args.messages)]
    for account, uid, cid, results in zip(accounts, uids, cids, messages):
        for ordinal, (rc, text) in enumerate(results, 1):
            check(f"agent {account} message {ordinal} got a CLI delivery acknowledgement",
                  rc == 0 and identity(text) == uid and identity(text, "X-UFSRVCID") == cid and
                  len(re.findall(r"group message: POST .*\(fid=" + fid + r"\)", text)) == 1 and
                  bool(re.search(r"group message: delivered to fid=" + fid + r"\b", text)))
    api = window(args.api_log, args.api_container, api_before)
    ws = window(args.log, args.container, ws_before)
    (workdir / "api.log").write_text(api)
    (workdir / "websock.log").write_text(ws)
    if args.evidence:
        args.evidence.parent.mkdir(parents=True, exist_ok=True)
        args.evidence.write_text(ws)
    queue = f"STAGED_INTRAMSGS_{args.fence_class}_{args.geogroup}"
    for kind, topic, pattern, expected_count in (
        ("join", "FENCE", r"reqid:'(\d+)', group_name:'" + re.escape(name) + r"'.*Publishing Fence intra-message", args.agents),
        ("message", "MSG", r"reqid:'(\d+)', ctx_id:'" + fid + r"'.*Publishing Message intra-message", total),
    ):
        requests = re.findall(pattern, api)
        check(f"all API {kind} request IDs match websock staged processing",
              intra_requests_processed(requests, ws, topic, expected_count))
        check(f"no run {kind} remains in the INTRA staging queue",
              len(requests) == expected_count and
              all(redis(args.redis_usrmsg, "ZCOUNT", queue, rid, rid) == "0" for rid in requests))
    records = re.findall(r"INSERT INTO messages \(id_events, fid, type, rawmsg, timestamp, originator, originator_device\)"
                         r" VALUES \('(\d+)', '" + fid + r"', '\d+', '[^']*', '\d+', '(\d+)', '\d+'\)", ws)
    check("unique message insert records for each sender",
          len({gid for gid, _ in records}) == total and Counter(uid for _, uid in records) ==
          Counter({uid: args.messages for uid in uids}) and not any(
              "ERROR: COULD NOT EXECUTE QUERY" in line and "INSERT INTO messages" in line
              and f"', '{fid}'," in line for line in ws.splitlines()))
    dispatches = re.findall(r"_MarshalFenceMessageToUser \{[^\n]*?cid:'(\d+)', cid_target:'(\d+)'"
                            r"[^\n]*fid:'" + fid + r"'", ws)
    check("websock dispatched each sender's messages to every participating session",
          Counter(dispatches) == Counter({(sender, recipient): args.messages
                                          for sender in cids for recipient in cids}))
    after = event_counter(fid)
    check(f"message event counter {before} -> {after}, expected {before + total}", after == before + total)
    for uid in uids:
        for gid, _ in records:
            member = f"{fid}:{gid}"
            indexed = score(args.redis_usrmsg, f"STAGED_OUTMSG_EVENTS:{uid}", member)
            key = f"STAGED_OUTMSG:{uid}"
            payload = redis(args.redis_usrmsg, "HEXISTS", key, member)
            if indexed == "":
                paired = payload == "0"
            else:
                size = redis(args.redis_usrmsg, "HSTRLEN", key, member)
                paired = (payload == "1" and indexed.isdigit() and int(indexed) > 0
                          and size.isdigit() and int(size) > 0)
            check(f"outbound staging pair uid={uid} gid={gid} (indexed={bool(indexed)}, payload={payload})", paired)
    check_fence(fid, cname, creators[0], uids)
except (OSError, ValueError, subprocess.SubprocessError) as exc:
    print(f"FAIL ({phase}): {exc}", file=sys.stderr)
    sys.exit(2 if phase == "preflight" else 1)

print(f"Workers: requested={args.workers} launched={worker_count}; join workers={min(worker_count, args.agents)}")
if args.workers > worker_count:
    print("Worker count reduced to the number of message operations.")
print(f"{'PASS' if all(checks) else 'FAIL'} shared fence and message proof. Evidence: {workdir}")
sys.exit(0 if all(checks) else 1)
PY
