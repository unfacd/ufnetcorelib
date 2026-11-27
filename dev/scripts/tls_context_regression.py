#!/usr/bin/env python3
"""Static correctness proof for per-thread handle storage (TLS vs pthread keys).

Scans a worker's syslog for the per-thread initialisation records, then reads
the live process's `ThreadContext` blocks with lldb and cross-correlates every
logged address against what the process actually holds.

Checks, per thread:
  slots    startup inventory agrees with the addresses each worker bound in TLS
  live     the logged TLS address is readable in the live process
  init     the block is populated (worker roles must be; others are n/a)
  seed     ThreadContext.random_state equals the thread's logged seed
  fs       the TLS address is reachable from that thread's fs_base
  http     logged HttpRequestContext pointer sits in the block
  instr    logged InstrumentationBackend pointer sits in the block
  usrmsg   logged UserMessageCacheBackend pointer sits in the block
  fence    logged FenceCacheBackend pointer sits in the block
  offset   each handle sits at the same block offset on every thread of a role
  unique   no handle address is shared between two threads

Exit status: 0 all applicable checks pass, 1 a check failed, 2 setup error.

Requires: python3 on the host. Live memory checks also require lldb or gdb
inside the target container. Use --slots-only to check the startup records alone.

Example:
  dev/scripts/tls_context_regression.py --container ufsrvapi --process ufsrvapi \
      --log /var/log/ufsrvapi.log --require-tls-reads

  dev/scripts/tls_context_regression.py --container ufsrvwebsock \
      --process ufsrvwebsock --log /var/log/ufsrvwebsock.log
"""

import argparse
import json
import re
import shlex
import subprocess
import sys

HEX = r"0x[0-9a-fA-F]+"

PAT_LAUNCH_SESSION = re.compile(
    r"ThreadWebSockets: --> Launching into main loop: pid:'(\d+)', "
    r"ufsrv_th_ctx:'(" + HEX + r")'.*random_state: '(\d+)'")
PAT_LAUNCH_JOB = re.compile(
    r"_ThreadUFServerWorker: --> Launching into main loop: pid:'(\d+)', "
    r"ufsrv_th_ctx:'(" + HEX + r")'")
PAT_REDIS_TLS = re.compile(
    r"BuildConnectionHandleForRedisBackend \{pid:'(\d+)', th_ctx:'(" + HEX + r")'")
PAT_HTTP = re.compile(
    r"SUCCESS \(http_ptr:'(" + HEX + r")'\): Initialised HttpRequestContext for "
    r"(\w[\w ]*?) [Tt]hread: '(\d+)'")
PAT_INSTR = re.compile(
    r"SUCCESS \(instr_ptr:'(" + HEX + r")'\): Initialised Instrumentation Backend for "
    r"(\w[\w ]*?) [Tt]hread: '(\d+)'")
PAT_USRMSG = re.compile(
    r"SUCCESS \((" + HEX + r")\): Initialised Cache Backend UserMessage for "
    r"(\w[\w ]*?) [Tt]hread: '(\d+)'")
PAT_FENCE = re.compile(
    r"\(" + HEX + r"\): SUCCESS: Initialised Cache Backend Fence for "
    r"(\w[\w ]*?) [Tt]hread: '(\d+)'")
PAT_FENCE_ADDR = re.compile(
    r"\((" + HEX + r")\): SUCCESS: Initialised Cache Backend Fence for "
    r"(\w[\w ]*?) [Tt]hread: '(\d+)'")
PAT_BOUNDARY = r"-{6,} \S+ \([0-9]"
BLOCK_WORDS = 16

ROLE_BY_LOGNAME = {
    "Session Worker": "session_worker",
    "Ufsrv Worker": "job_worker",
    "Main": "main",
}

STATUS_PASS = "PASS"
STATUS_FAIL = "FAIL"
STATUS_NA = "n/a"
SLOT_EVENTS = re.compile(r'"event"\s*:\s*"(worker_slot_inventory|worker_slot_bound|worker_db_initialisation_failed)"')
SLOT_HANDLES = ("user_data", "persistence_backend", "instrumentation_backend",
                "usrmsg_cachebackend", "fence_cachebackend", "msgqueue_backend",
                "db_backend", "ratelimit_backend")
REQUIRED_HANDLES = ("user_data", "persistence_backend", "usrmsg_cachebackend",
                    "fence_cachebackend", "ratelimit_backend")


class SetupError(Exception):
    pass


def run(cmd, **kw):
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, **kw)
    return proc.returncode, proc.stdout


def docker(*args, **kw):
    return run(["docker", *args], **kw)


def check_worker_slots(text, pid):
    records = []
    failures = []
    for line in text.splitlines():
        match = SLOT_EVENTS.search(line)
        if not match:
            continue
        try:
            record = json.loads(line[line.index("{"):])
        except ValueError:
            records.append((match.group(1), None))
            continue
        if record.get("pid") == int(pid):
            records.append((match.group(1), record))

    inventory_index = next((i for i in range(len(records) - 1, -1, -1)
                            if records[i][0] == "worker_slot_inventory"), None)
    if inventory_index is None:
        return {}, [], [("-", "missing worker slot inventory for PID %s" % pid)]
    inventory = records[inventory_index][1]
    if inventory is None:
        return {}, [], [("-", "invalid worker slot inventory JSON")]

    def slot_key(slot):
        if not isinstance(slot, dict):
            return None
        pool, index = slot.get("pool"), slot.get("slot_index")
        if pool in ("session", "job") and type(index) is int and index >= 0:
            return pool, index
        return None

    def address(value):
        return isinstance(value, str) and re.fullmatch(HEX, value) and int(value, 16) != 0

    def check_addresses(label, slot, required, seen):
        for field in SLOT_HANDLES:
            value = slot.get(field)
            if field not in slot or (value is None and field in required):
                failures.append((label, "missing %s" % field))
            elif value is not None:
                if not address(value):
                    failures.append((label, "invalid %s address %r" % (field, value)))
                    continue
                identity = (field, int(value, 16))
                if identity in seen:
                    failures.append((label, "%s %s shared with %s" % (field, value, seen[identity])))
                seen[identity] = label

    expected = {}
    seen_handles = {}
    delegator = inventory.get("delegator_persistence_backend")
    if address(delegator):
        seen_handles["persistence_backend", int(delegator, 16)] = "delegator"
    else:
        failures.append(("delegator", "missing or invalid persistence_backend"))

    slots = inventory.get("slots")
    if not isinstance(slots, list):
        return inventory, [], failures + [("-", "inventory slots must be an array")]
    for slot in slots:
        key = slot_key(slot)
        if key is None:
            failures.append(("-", "invalid inventory slot"))
            continue
        label = "%s[%d]" % key
        if key in expected:
            failures.append((label, "duplicate inventory slot"))
        expected[key] = slot
        required = REQUIRED_HANDLES + (("msgqueue_backend",) if key[0] == "job" else ())
        check_addresses(label, slot, required, seen_handles)

    for pool in ("session", "job"):
        count = inventory.get(pool + "_worker_count")
        indexes = sorted(index for role, index in expected if role == pool)
        minimum = 1 if pool == "session" else 0
        if (type(count) is not int or count < minimum or len(indexes) != count
                or any(i != index for i, index in enumerate(indexes))):
            failures.append((pool, "inventory does not cover indices 0 to count-1 (count=%r)" % count))

    reports = []
    bound = set()
    initializer_tid = inventory.get("thread_id")
    thread_ids = set()
    if type(initializer_tid) is int and initializer_tid > 0:
        thread_ids.add(initializer_tid)
    else:
        failures.append(("-", "missing or invalid initializer thread_id"))
    seen_handles = {}
    thread_contexts = set()
    for event, report in records[inventory_index + 1:]:
        if report is None:
            failures.append(("-", "invalid %s JSON" % event))
            continue
        if event != "worker_slot_bound":
            failures.append(("-", "%s for %r" % (event, slot_key(report))))
            continue
        reports.append(report)
        key = slot_key(report)
        if key not in expected:
            failures.append(("-", "unexpected worker slot %r" % (key,)))
            continue
        label = "%s[%d]" % key
        if key in bound:
            failures.append((label, "duplicate worker binding"))
        bound.add(key)
        tid = report.get("thread_id")
        if type(tid) is not int or tid <= 0 or tid in thread_ids:
            failures.append((label, "missing, invalid, or duplicate thread_id %r" % tid))
        else:
            thread_ids.add(tid)
        context = report.get("thread_context")
        if not address(context):
            failures.append((label, "missing or invalid thread_context"))
        elif int(context, 16) in thread_contexts:
            failures.append((label, "duplicate thread_context %s" % context))
        else:
            thread_contexts.add(int(context, 16))
        check_addresses(label, report, REQUIRED_HANDLES + ("db_backend",), seen_handles)
        for field in SLOT_HANDLES:
            value = expected[key].get(field)
            if field == "db_backend" and value is None:
                continue
            if report.get(field) != value:
                failures.append((label, "%s expected %s, worker reported %s" %
                                 (field, value, report.get(field))))

    for key in sorted(expected.keys() - bound):
        failures.append(("%s[%d]" % key, "missing worker binding"))
    return inventory, reports, failures


class Thread(object):
    def __init__(self, tid):
        self.tid = tid
        self.role = None
        self.tls = None
        self.base = None
        self.seed = None
        self.http = None
        self.instr = None
        self.usrmsg = None
        self.fence = None
        self.words = None
        self.fs_base = None
        self.checks = {}

    def as_dict(self):
        return {
            "tid": self.tid, "role": self.role, "tls": self.tls,
            "base": self.base, "seed": self.seed, "http": self.http,
            "instr": self.instr, "usrmsg": self.usrmsg, "fence": self.fence,
            "fs_base": self.fs_base,
            "checks": self.checks,
        }


def scan_log(text, boundary, reports=()):
    start = 0
    matches = list(re.finditer(boundary, text)) if boundary else []
    if matches:
        start = matches[-1].start()
    text = text[start:]

    threads = {}
    restart_hits = {}

    def get(tid):
        if tid not in threads:
            threads[tid] = Thread(tid)
        return threads[tid]

    def set_once(th, field, value, role=None):
        current = getattr(th, field)
        if current is not None and current != value:
            restart_hits.setdefault((th.tid, field), set()).update((current, value))
        setattr(th, field, value)
        if role and th.role is None:
            th.role = role

    for line in text.splitlines():
        m = PAT_LAUNCH_SESSION.search(line)
        if m:
            th = get(m.group(1))
            set_once(th, "tls", m.group(2), "session_worker")
            th.seed = int(m.group(3))
            continue
        m = PAT_LAUNCH_JOB.search(line)
        if m:
            set_once(get(m.group(1)), "base", m.group(2), "job_worker")
            continue
        m = PAT_REDIS_TLS.search(line)
        if m:
            set_once(get(m.group(1)), "tls", m.group(2))
            continue
        m = PAT_HTTP.search(line)
        if m:
            set_once(get(m.group(3)), "http", m.group(1),
                     ROLE_BY_LOGNAME.get(m.group(2)))
            continue
        m = PAT_INSTR.search(line)
        if m:
            set_once(get(m.group(3)), "instr", m.group(1),
                     ROLE_BY_LOGNAME.get(m.group(2)))
            continue
        m = PAT_USRMSG.search(line)
        if m:
            set_once(get(m.group(3)), "usrmsg", m.group(1),
                     ROLE_BY_LOGNAME.get(m.group(2)))
            continue
        m = PAT_FENCE_ADDR.search(line)
        if m:
            set_once(get(m.group(3)), "fence", m.group(1),
                     ROLE_BY_LOGNAME.get(m.group(2)))

    for report in reports:
        tid = report.get("thread_id")
        tls = report.get("thread_context")
        pool = report.get("pool")
        if type(tid) is not int or not isinstance(tls, str) or not re.fullmatch(HEX, tls):
            continue
        if pool not in ("session", "job"):
            continue
        th = get(str(tid))
        set_once(th, "tls", tls, pool + "_worker")
        th.usrmsg = report.get("usrmsg_cachebackend")
        th.fence = report.get("fence_cachebackend")
        for field in ("usrmsg", "fence"):
            value = getattr(th, field)
            if not isinstance(value, str) or not re.fullmatch(HEX, value):
                setattr(th, field, None)

    for th in threads.values():
        if th.role is None:
            th.role = "auxiliary"
    return [t for t in threads.values() if t.tls], restart_hits


def container_pid(container, process):
    rc, out = docker("exec", container, "sh", "-c",
                     "pgrep -x %s | head -1" % shlex.quote(process))
    if rc != 0 or not out.strip():
        raise SetupError("no process %r in container %r" % (process, container))
    return out.strip()


def binary_imports(container, binary):
    rc, out = docker("exec", container, "sh", "-c",
                     "nm -D %s 2>/dev/null | grep -c pthread_getspecific" % shlex.quote(binary))
    return out.strip() != "0"


def thread_count(container, pid):
    rc, out = docker("exec", container, "sh", "-c",
                     "ls /proc/%s/task 2>/dev/null | wc -l" % pid)
    try:
        return int(out.strip())
    except ValueError:
        return 0


def read_live(container, pid, debugger, tls_addrs, nthreads):
    commands = ["thread list"]
    for idx in range(1, nthreads + 1):
        commands.append("thread select %d" % idx)
        commands.append("register read fs_base")
    for addr in tls_addrs:
        commands.append("memory read -f x -s 8 -c 16 %s" % addr)
    commands.append("detach")

    if debugger == "lldb":
        argv = ["lldb", "-b"]
        for c in commands:
            argv += ["-o", c]
        argv += ["-p", pid]
    else:
        argv = ["gdb", "-q", "-batch"]
        for c in commands:
            argv += ["-ex", c]
        argv += ["-p", pid]

    rc, out = docker("exec", container, *argv)

    blocks = {}
    mem = {}
    fs_bases = []
    requested = [int(a, 16) for a in tls_addrs]
    current = None
    for line in out.splitlines():
        m = re.search(r"thread select (\d+)", line)
        if m:
            current = int(m.group(1))
        m = re.search(r"fs_base\s*=\s*(" + HEX + r")", line)
        if m and current is not None:
            fs_bases.append(int(m.group(1), 16))
        m = re.search(r"^\s*(" + HEX + r"):\s*((?:" + HEX + r"\s*)+)$", line)
        if m:
            words = [int(w, 16) for w in m.group(2).split()]
            mem[int(m.group(1), 16)] = words

    for base in requested:
        words = []
        for step in range(0, BLOCK_WORDS * 8, 16):
            words.extend(mem.get(base + step, [0, 0]))
        blocks[base] = words[:BLOCK_WORDS]

    return blocks, fs_bases, out


def cross_correlate(threads, blocks, fs_bases, restarts):
    by_role = {}
    for th in threads:
        th.words = blocks.get(int(th.tls, 16))
        th.checks["live"] = STATUS_PASS if th.words else STATUS_FAIL
        th.checks["init"] = (
            STATUS_NA if th.words is None else
            (STATUS_PASS if any(th.words) else
             (STATUS_FAIL if th.role in ("session_worker", "job_worker") else STATUS_NA)))
        if th.words and th.seed is not None:
            th.checks["seed"] = STATUS_PASS if (th.words[0] & 0xffffffff) == th.seed else STATUS_FAIL
        else:
            th.checks["seed"] = STATUS_NA
        by_role.setdefault(th.role, []).append(th)

    off = None
    if fs_bases:
        votes = {}
        for th in threads:
            for fb in fs_bases:
                d = int(th.tls, 16) - fb
                votes[d] = votes.get(d, 0) + 1
        if votes:
            off = max(votes, key=votes.get)
    for th in threads:
        th.checks["fs"] = STATUS_NA
        if off is None or not th.words:
            continue
        best = min(fs_bases, key=lambda fb: abs((fb + off) - int(th.tls, 16)))
        th.fs_base = hex(best)
        th.checks["fs"] = STATUS_PASS if best + off == int(th.tls, 16) else STATUS_FAIL

    field_names = ("http", "instr", "usrmsg", "fence")
    measured = {}
    for role, members in by_role.items():
        for field in field_names:
            offsets = {}
            for th in members:
                if role not in ("session_worker", "job_worker") or th.words is None:
                    th.checks[field] = STATUS_NA
                    continue
                target = getattr(th, field)
                if target is None:
                    th.checks[field] = STATUS_NA
                    continue
                hits = [i for i, w in enumerate(th.words) if w == int(target, 16)]
                if len(hits) != 1:
                    th.checks[field] = STATUS_FAIL
                    continue
                offsets[th.tid] = hits[0]
                th.checks[field] = STATUS_PASS
            measured.setdefault(field, set()).update(offsets.values())
            if len(set(offsets.values())) == 1:
                for th in members:
                    if th.checks.get(field) == STATUS_PASS:
                        th.checks["offset_" + field] = STATUS_PASS
            elif offsets:
                for th in members:
                    if th.checks.get(field) == STATUS_PASS:
                        th.checks["offset_" + field] = STATUS_FAIL
            for th in members:
                th.checks.setdefault("offset_" + field, STATUS_NA)

        for field in field_names:
            values = [getattr(th, field) for th in members if getattr(th, field)]
            for th in members:
                if role not in ("session_worker", "job_worker") or getattr(th, field) is None:
                    th.checks["unique_" + field] = STATUS_NA
                else:
                    th.checks["unique_" + field] = (
                        STATUS_PASS if values.count(getattr(th, field)) == 1 else STATUS_FAIL)

        tls_values = [th.tls for th in members]
        for th in members:
            th.checks["unique_tls"] = (
                STATUS_PASS if tls_values.count(th.tls) == 1 else STATUS_FAIL)


    layout = {}
    for field, offsets in measured.items():
        if len(offsets) == 1:
            layout[field] = next(iter(offsets))

    workers = [t for t in threads if t.role in ("session_worker", "job_worker") and t.words]
    for field, offset in layout.items():
        values = [t.words[offset] for t in workers if len(t.words) > offset]
        for th in workers:
            if len(th.words) <= offset:
                continue
            value = th.words[offset]
            if th.checks.get(field) in (None, STATUS_NA):
                th.checks[field] = STATUS_PASS if value else STATUS_FAIL
            if th.checks.get("unique_" + field) in (None, STATUS_NA):
                th.checks["unique_" + field] = (
                    STATUS_PASS if values.count(value) == 1 and value else STATUS_FAIL)

    for (tid, field), values in restarts.items():
        for th in threads:
            if th.tid == tid:
                th.checks["restart_" + field] = STATUS_FAIL

    return off, layout


def fmt(value):
    return "-" if value is None else value


def print_matrix(threads, off, layout):
    fields = ["http", "instr", "usrmsg", "fence"]
    columns = ["live", "init", "seed", "fs"] + fields + ["offset_" + f for f in fields] + \
              ["unique_tls"] + ["unique_" + f for f in fields]
    short = {
        "live": "live", "init": "init", "seed": "seed", "fs": "fs",
        "http": "http", "instr": "instr", "usrmsg": "usrmsg", "fence": "fence",
        "offset_http": "o:http", "offset_instr": "o:instr",
        "offset_usrmsg": "o:usrmsg", "offset_fence": "o:fence",
        "unique_tls": "u:tls", "unique_http": "u:http", "unique_instr": "u:instr",
        "unique_usrmsg": "u:usrmsg", "unique_fence": "u:fence",
    }
    glyph = {STATUS_PASS: " ok ", STATUS_FAIL: "FAIL", STATUS_NA: "  - "}
    header = ["tid", "role", "tls"] + [short[c] for c in columns]
    print("  ".join(header))
    print("  ".join("-" * len(h) for h in header))
    for th in threads:
        row = [th.tid, th.role, th.tls]
        row += [glyph.get(th.checks.get(c, STATUS_NA), "  ? ") for c in columns]
        print("  ".join(row))
    print()
    print("threads: %d   fs_base offset: %s   layout: %s" % (
        len(threads),
        hex(off) if off is not None else "n/a",
        ", ".join("%s=+0x%x" % (k, v * 8) for k, v in sorted(layout.items()))))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--container", default="ufsrvapi")
    ap.add_argument("--log", default="/var/log/ufsrvapi.log")
    ap.add_argument("--pid", default=None, help="process pid inside the container")
    ap.add_argument("--process", default="ufsrvapi", help="process name for pid lookup")
    ap.add_argument("--debugger", default="lldb", choices=["lldb", "gdb"])
    ap.add_argument("--boundary", default=PAT_BOUNDARY,
                    help="regex marking the start of the run to scan (last match wins)")
    ap.add_argument("--require-tls-reads", action="store_true",
                    help="fail if the binary still imports pthread_getspecific")
    ap.add_argument("--slots-only", action="store_true",
                    help="check worker slot inventory and bindings without attaching a debugger")
    ap.add_argument("--json", default=None, help="write the full result as json")
    args = ap.parse_args()

    try:
        pid = args.pid or container_pid(args.container, args.process)
        rc, out = docker("exec", args.container, "sh", "-c", "cat %s" % shlex.quote(args.log))
        if rc != 0:
            raise SetupError("cannot read %s in %s" % (args.log, args.container))
        inventory, reports, slot_failures = check_worker_slots(out, pid)
        slots = inventory.get("slots")
        print("worker slots: %d expected, %d reported, %d failures" %
              (len(slots) if isinstance(slots, list) else 0, len(reports), len(slot_failures)))
        for label, failure in slot_failures:
            print("  %s: %s" % (label, failure))
        if args.slots_only:
            if args.json:
                with open(args.json, "w") as fh:
                    json.dump({"pid": pid, "inventory": inventory, "bindings": reports,
                               "failures": slot_failures}, fh, indent=2)
            return int(bool(slot_failures))
        threads, restarts = scan_log(out, args.boundary, reports)
        if not threads:
            raise SetupError("no thread initialisation records found in %s" % args.log)
        rc, exe = docker("exec", args.container, "sh", "-c",
                         "readlink /proc/%s/exe" % pid)
        binary = exe.strip() or "?"
        old_path = binary_imports(args.container, binary) if binary != "?" else None
        nthreads = thread_count(args.container, pid) or 64
        blocks, fs_bases, raw = read_live(args.container, pid, args.debugger,
                                          [t.tls for t in threads], nthreads)
        if not blocks:
            raise SetupError("live memory read returned nothing\n%s" % raw[-2000:])
    except SetupError as exc:
        print("setup error: %s" % exc, file=sys.stderr)
        return 2

    off, layout = cross_correlate(threads, blocks, fs_bases, restarts)
    threads.sort(key=lambda t: (t.role, t.tid))
    print_matrix(threads, off, layout)

    failures = list(slot_failures)
    for th in threads:
        for check, status in th.checks.items():
            if status == STATUS_FAIL:
                failures.append((th.tid, check))
        if th.role in ("session_worker", "job_worker") and th.checks.get("init") == STATUS_NA:
            failures.append((th.tid, "init"))

    print("binary: %s" % binary)
    print("pthread_getspecific import: %s" % (
        "present (pre-conversion build)" if old_path else
        "absent (TLS-only build)" if old_path is False else "unknown"))

    if args.require_tls_reads and old_path:
        failures.append(("-", "tls_reads_only"))

    if args.json:
        with open(args.json, "w") as fh:
            json.dump({"pid": pid, "binary": binary,
                       "imports_getspecific": old_path,
                       "fs_base_offset": hex(off) if off is not None else None,
                       "threads": [t.as_dict() for t in threads],
                       "inventory": inventory, "bindings": reports,
                       "failures": failures}, fh, indent=2)

    if failures:
        print("\nFAILURES:")
        for tid, check in failures:
            print("  %s: %s" % (tid, check))
        return 1
    print("\nRESULT: PASS (%d threads, %d checks)" % (len(threads), sum(len(t.checks) for t in threads)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
