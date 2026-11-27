import argparse
import base64
from collections import Counter, deque
import json
import math
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import sys
import tempfile
import time

from state_sync_proof import lua_value, uid_bytes


class DeliveryLedger:
    def __init__(self, identities, messages, run, binary=False):
        self.identities = identities
        self.ready = {}
        self.joined = {}
        self.acks = {}
        self.received = {}
        self.self_echoes = {}
        self.left = set()
        self.stopped = set()
        self.outside_scope = 0
        self.fence_name = None
        self.tokens = set()
        self.raw_messages = Counter()
        self.fid = None
        self.payloads = {f'{run}:a{account}:m{ordinal}': (uid, f'{run}:a{account}:m{ordinal}'.encode() + (b'\x00\xff' if binary else b''))
                         for account, uid in identities.items() for ordinal in range(1, messages + 1)}
        self.tags = {payload: tag for tag, (_, payload) in self.payloads.items()}
        self.messages = messages

    @staticmethod
    def identifier(event, name):
        value = event.get(name)
        if not isinstance(value, str) or not value.isdecimal() or int(value) <= 0:
            raise ValueError(f'invalid {name} in CLI event')
        return int(value)

    def observe(self, account, event):
        uid = self.identities[account]
        if event.get('event') == 'error':
            raise ValueError(f'account {account}: {event.get("error", "CLI session error")}')
        if self.identifier(event, 'uid') != uid:
            raise ValueError(f'account {account}: event belongs to another UID')
        kind = event.get('event')
        if kind == 'error':
            raise ValueError(f'account {account}: {event.get("error", "CLI session error")}')
        if kind == 'ready':
            if account in self.ready:
                raise ValueError('duplicate ready event')
            self.ready[account] = self.identifier(event, 'cid')
            return
        if account not in self.ready or self.identifier(event, 'cid') != self.ready[account]:
            raise ValueError('connection identity changed or event preceded authentication')
        if kind == 'joined':
            if event.get('id') != f'join_{account}' or (self.fence_name is not None and event.get('name') != self.fence_name):
                raise ValueError('join command or fence name mismatch')
            fid = self.identifier(event, 'fid')
            if account in self.joined or (self.fid is not None and self.fid != fid):
                raise ValueError('duplicate join or clients joined different fences')
            self.fid = fid
            self.joined[account] = event
        elif kind == 'ack':
            tag = event.get('id')
            if tag not in self.payloads or self.payloads[tag][0] != uid or tag in self.acks:
                raise ValueError('unexpected or duplicate sender acknowledgement')
            if self.identifier(event, 'fid') != self.fid:
                raise ValueError('acknowledgement targets the wrong fence')
            token = (uid, self.identifier(event, 'when_client'))
            if token in self.tokens:
                raise ValueError('sender reused a request echo token')
            self.tokens.add(token)
            gid = self.identifier(event, 'gid')
            if gid in self.acks.values():
                raise ValueError('different sends share one message ID')
            self.acks[tag] = gid
        elif kind == 'message':
            self.raw_messages[account] += 1
            try:
                payload = base64.b64decode(event['body_base64'], validate=True)
            except (ValueError, KeyError, TypeError) as error:
                raise ValueError('invalid received payload encoding') from error
            fid = self.identifier(event, 'fid')
            tag = self.tags.get(payload)
            if fid != self.fid:
                if tag is not None:
                    raise ValueError('test payload received on the wrong fence')
                self.outside_scope += 1
                return
            if tag is None:
                raise ValueError('unexpected or corrupted payload in the test fence')
            if event.get('protocol') != 1 or event.get('outcome') != 'SYNCED':
                raise ValueError('unexpected received message protocol or outcome')
            sender = self.identifier(event, 'sender_uid')
            if sender != self.payloads[tag][0]:
                raise ValueError('payload attributed to the wrong sender')
            key = (uid, tag)
            records = self.self_echoes if uid == sender else self.received
            if key in records:
                raise ValueError('duplicate receiver delivery')
            records[key] = self.identifier(event, 'gid')
        elif kind == 'ack_unmatched':
            if self.fid is not None and event.get('fid') == str(self.fid):
                raise ValueError('duplicate or unmatched acknowledgement for the test fence')
            self.outside_scope += 1
        elif kind == 'left':
            if account in self.left or self.identifier(event, 'fid') != self.fid:
                raise ValueError('duplicate or mismatched leave')
            if event.get('id') != f'leave_{account}':
                raise ValueError('leave command correlation mismatch')
            self.left.add(account)
        elif kind == 'stopped':
            if account in self.stopped:
                raise ValueError('duplicate stopped event')
            if event.get('id') != f'stop_{account}' or event.get('acknowledged') != self.messages or event.get('received') != self.raw_messages[account]:
                raise ValueError('stop command or client counters disagree with observed events')
            self.stopped.add(account)
        else:
            raise ValueError(f'unknown CLI event: {kind}')

    def missing(self):
        return [(uid, tag) for uid in self.identities.values()
                for tag, (sender, _) in self.payloads.items()
                if sender != uid and (uid, tag) not in self.received]

    def verify(self):
        if len(self.joined) != len(self.identities) or len(self.acks) != len(self.payloads) or self.missing():
            raise ValueError('joins, sender acknowledgements or peer deliveries are incomplete')
        for (_, tag), gid in {**self.received, **self.self_echoes}.items():
            if gid != self.acks[tag]:
                raise ValueError('receiver message ID differs from sender acknowledgement')

    def summary(self):
        return dict(sent_expected=len(self.payloads), acknowledged=len(self.acks),
                    peer_deliveries_expected=len(self.payloads) * (len(self.identities) - 1),
                    peer_deliveries=len(self.received), missing=[dict(uid=uid, tag=tag) for uid, tag in self.missing()],
                    self_echoes=len(self.self_echoes), outside_scope=self.outside_scope,
                    accounts=[dict(account=account, uid=uid, joined=account in self.joined,
                                   acknowledged=sum(self.payloads[tag][0] == uid for tag in self.acks),
                                   received=sum(recipient == uid for recipient, _ in self.received),
                                   expected=(len(self.identities) - 1) * self.messages)
                              for account, uid in self.identities.items()])


def run(args, evidence, report):
    identities, cookies = {}, set()
    for account in range(args.account, args.account + args.agents):
        original = args.home_base / str(account)
        text = (original / 'ufsrvcli.prefs').read_text()
        uid = int.from_bytes(uid_bytes(lua_value(text, 'ufsrvuid'))[8:], 'little')
        cookie = lua_value(text, 'cookie')
        if not uid or uid in identities.values() or not cookie or cookie in cookies:
            raise ValueError('participants require distinct nonempty UIDs and cookies')
        identities[account] = uid
        cookies.add(cookie)
    ledger = DeliveryLedger(identities, args.messages, evidence.name, args.binary_payload)
    report['identities'] = identities
    clients = {}
    selector = selectors.DefaultSelector()
    deadline = time.monotonic() + args.timeout
    workers = min(args.workers, args.agents)
    report['workers'] = dict(requested=args.workers, launched=workers)
    report['passed'] = False

    def send(account, command):
        client = clients[account]
        client['proc'].stdin.write((json.dumps(command) + '\n').encode())
        client['proc'].stdin.flush()
        client['commands'].write(json.dumps(command) + '\n')
        client['commands'].flush()

    def pump(wait=0.1):
        if time.monotonic() >= deadline:
            raise TimeoutError('persistent message proof deadline expired')
        for key, _ in selector.select(min(wait, max(0, deadline - time.monotonic()))):
            account = key.data
            client = clients[account]
            data = os.read(key.fileobj.fileno(), 65536)
            if not data and account in ledger.stopped:
                if client['buffer']:
                    raise ValueError('truncated CLI event at shutdown')
                selector.unregister(key.fileobj)
                continue
            if not data:
                raise ValueError(f'account {account}: CLI event stream closed before completion (exit={client["proc"].poll()})')
            client['buffer'] += data
            if len(client['buffer']) > 1048576:
                raise ValueError('oversized CLI event')
            while b'\n' in client['buffer']:
                line, client['buffer'] = client['buffer'].split(b'\n', 1)
                client['log'].write(line + b'\n')
                client['log'].flush()
                if not line.startswith(b'{'):
                    continue
                event = json.loads(line)
                client['events'].write(json.dumps(event) + '\n')
                client['events'].flush()
                ledger.observe(account, event)
        for account, client in clients.items():
            if account not in ledger.stopped and client['proc'].poll() is not None:
                raise ValueError(f'account {account}: CLI exited before completion (exit={client["proc"].returncode})')

    def until(predicate):
        while not predicate():
            pump()

    try:
        for account in identities:
            directory = evidence / str(account)
            directory.mkdir(mode=0o700)
            home = directory / 'profile'
            home.mkdir(mode=0o700)
            for name in ('ufsrvcli.config', 'ufsrvcli.prefs'):
                shutil.copyfile(args.home_base / str(account) / name, home / name)
                (home / name).chmod(0o600)
            log = (directory / 'cli.log').open('wb')
            proc = subprocess.Popen([args.cli, '--headless', '--command-stream', '--home-base', str(home),
                                     '--session-timeout-ms', str(math.ceil(args.timeout * 1000))],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
            os.set_blocking(proc.stdout.fileno(), False)
            clients[account] = dict(proc=proc, log=log, buffer=b'', events=(directory / 'events.jsonl').open('w'),
                                    commands=(directory / 'commands.jsonl').open('w'))
            selector.register(proc.stdout, selectors.EVENT_READ, account)
            until(lambda: account in ledger.ready)
            if account != list(identities)[-1]:
                end = time.monotonic() + args.launch_spacing
                while time.monotonic() < end:
                    pump(min(0.1, end - time.monotonic()))
        fence_name = f'{args.prefix}_{evidence.name}'
        report['fence_name'] = fence_name
        ledger.fence_name = fence_name
        for account in identities:
            send(account, dict(id=f'join_{account}', op='join', name=fence_name))
            until(lambda: account in ledger.joined)
        report['fid'] = str(ledger.fid)
        operations = deque((account, tag) for ordinal in range(1, args.messages + 1)
                           for account in identities
                           for tag in [f'{evidence.name}:a{account}:m{ordinal}'])
        active = {}
        while operations or active:
            for account, tag in list(active.items()):
                if tag in ledger.acks:
                    del active[account]
            attempts = len(operations)
            while operations and len(active) < workers and attempts:
                attempts -= 1
                account, tag = operations.popleft()
                if account in active:
                    operations.append((account, tag))
                    continue
                active[account] = tag
                send(account, dict(id=tag, op='message', fid=str(ledger.fid),
                                   body_base64=base64.b64encode(ledger.payloads[tag][1]).decode()))
            if operations or active:
                pump()
        until(lambda: not ledger.missing())
        ledger.verify()
        settle = time.monotonic() + args.settle_ms / 1000
        while time.monotonic() < settle:
            pump(min(0.1, settle - time.monotonic()))
        ledger.verify()
        report['delivery_verified'] = True
        for account in identities:
            send(account, dict(id=f'leave_{account}', op='leave', fid=str(ledger.fid)))
            until(lambda: account in ledger.left)
        for account in identities:
            send(account, dict(id=f'stop_{account}', op='stop'))
        until(lambda: len(ledger.stopped) == args.agents)
        for account, client in clients.items():
            code = client['proc'].wait(timeout=max(0.1, deadline - time.monotonic()))
            client['exit'] = code
            if code != 0:
                raise ValueError(f'account {account}: CLI exit={code}; inspect cli.log')
        report['passed'] = True
    finally:
        report['summary'] = ledger.summary()
        report['acknowledgements'] = ledger.acks
        report['deliveries'] = [dict(uid=uid, tag=tag, gid=str(gid))
                                for (uid, tag), gid in ledger.received.items()]
        report['cleanup'] = dict(left=sorted(ledger.left), stopped=sorted(ledger.stopped),
                                 remaining_accounts=[a for a in identities if a in ledger.joined and a not in ledger.left],
                                 fid=str(ledger.fid) if ledger.fid is not None else None)
        for account, client in clients.items():
            proc = client['proc']
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            report.setdefault('cli_exits', {})[account] = proc.returncode
            for name in ('log', 'events', 'commands'):
                client[name].close()
            proc.stdin.close()
            proc.stdout.close()
        selector.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description='Independent persistent-client peer delivery correctness proof')
    parser.add_argument('--home-base', type=Path, required=True)
    parser.add_argument('--account', type=int, default=1)
    parser.add_argument('--agents', type=int, default=2)
    parser.add_argument('--messages', type=int, default=1)
    parser.add_argument('--workers', type=int, default=1)
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--settle-ms', type=int, default=500)
    parser.add_argument('--launch-spacing', type=float, default=1.2)
    parser.add_argument('--prefix', default='peerproof')
    parser.add_argument('--cli', default=str(Path(__file__).resolve().parents[3] / 'ufsrvcli/build/dev/src/ufsrvcli'))
    parser.add_argument('--local', action='store_true', help='endpoints still come from profiles; no Docker access in this proof')
    parser.add_argument('--print-invariants', action='store_true')
    parser.add_argument('--binary-payload', action='store_true', help='append NUL/0xff bytes for an explicit binary-content probe')
    parser.add_argument('--evidence-dir', type=Path, default=Path('/tmp/concurrency_correctness_proofs'))
    args = parser.parse_args(argv)
    if min(args.account, args.messages, args.workers) < 1 or args.agents < 2 or not math.isfinite(args.timeout) or not 0 < args.timeout <= 2147483:
        parser.error('positive account/messages/workers and timeout required; agents must be at least 2')
    if not math.isfinite(args.launch_spacing) or args.launch_spacing < 0 or args.settle_ms < 1:
        parser.error('launch spacing must be finite/nonnegative; settle-ms must be positive')
    os.umask(0o077)
    args.evidence_dir.mkdir(parents=True, exist_ok=True)
    evidence = Path(tempfile.mkdtemp(prefix='peer_messages_', dir=args.evidence_dir))
    report = dict(passed=False, started=time.time(), agents=args.agents, messages=args.messages,
                  verification='decoded native client callbacks; no server-log or Redis delivery inference')
    print(f'Evidence directory: {evidence}', flush=True)
    if args.print_invariants:
        print(f'Expected: {args.agents} persistent identities, {args.agents * args.messages} acknowledgements, '              f'{args.agents * (args.agents - 1) * args.messages} exact peer deliveries, zero duplicates, '              'stable CID per client and verified leave/stop with clean process exits.', flush=True)
    report['binary_payload'] = args.binary_payload
    try:
        run(args, evidence, report)
    except (Exception, KeyboardInterrupt) as error:
        report['error'] = str(error) or 'interrupted'
        report['passed'] = False
        print(f'FAIL: {report["error"]}', flush=True)
    report['finished'] = time.time()
    (evidence / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    for row in report.get('summary', {}).get('accounts', []):
        print(f'account={row["account"]} uid={row["uid"]} joined={row["joined"]} acks={row["acknowledged"]}/{args.messages} received={row["received"]}/{row["expected"]}')
    if 'workers' in report:
        print(f'Workers: requested={report["workers"]["requested"]} launched={report["workers"]["launched"]}; persistent clients={args.agents}')
        if args.workers > args.agents:
            print('Worker count capped: at most one pending operation per persistent client.')
    print(f'{"PASS" if report["passed"] else "FAIL"}: report={evidence / "report.json"}')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
