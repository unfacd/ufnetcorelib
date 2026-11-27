#!/usr/bin/env python3
"""Fence rename (FNAME) correctness proof.

Three scenarios, each on a fresh fence, driven through persistent `--command-stream`
clients:

  single  one member joins, renames the fence, and must observe the name change
          on its own connection;
  multi   several members join, one renames, and every member must observe the
          same change;
  same    several members join, one renames the fence to the title it already
          has, and the server must refuse it: the issuing member observes a
          rejection, no other member observes anything, and the fence is left
          untouched.

The client events are the evidence for delivery; the fence cache is the evidence
for persistence. After each member has confirmed the change, and again after the
CLI processes have exited, the fence record, the canonical registry and the name
index are read back from the fence Redis backend: they must carry the new name
after an applied rename, and must be unchanged by a refused one.
"""
import argparse
from collections import Counter
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
import unicodedata

sys.path.insert(0, str(Path(__file__).resolve().parent))
from state_sync_proof import lua_value, uid_bytes

FNAME_OPERATION = 4  # UnfacdFenceOperation UNFACD_FENCE_FNAME
OUTCOME_FNAME_APPLIED = ('UPDATED', 'ACCEPTED')
OUTCOME_FNAME_OWN = 'ACCEPTED'  # the originator's own copy; UPDATED is the fan-out to the others
OUTCOME_FNAME_REFUSED = 'REJECTED'
FNAME_ERROR_ATTRIBUTE_IDENTICAL = 18  # FENCE_COMMAND__ERRORS__ATTRIBUTE_IDENTICAL
FNAME_ERROR_PERMISSIONS_ADMIN = 3  # FENCE_COMMAND__ERRORS__PERMISSIONS_ADMIN: only the owner renames
STAGED_GRACE_SECONDS = 5.0  # the drain logs after the delivery; do not spin to the scenario deadline


class RenameLedger:
    """What the clients reported about one rename, and what it must add up to."""

    def __init__(self, identities, join_name, rename_id, new_name, renamer, expect_applied=True,
                 concurrent=False):
        self.identities = identities
        self.join_name = join_name
        self.rename_id = rename_id
        self.new_name = new_name
        self.renamer = renamer
        self.expect_applied = expect_applied
        self.concurrent = concurrent
        self.ready = {}
        self.joined = {}
        self.renamed = {}
        self.pushes = {}
        self.refused = {}
        self.left = set()
        self.stopped = set()
        self.fid = None
        self.when_client = None

    def request_id(self, account):
        """Correlation id the account used for its request."""
        return f'rename_{account}' if self.concurrent else self.rename_id

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

        if kind == 'ready':
            if account in self.ready:
                raise ValueError('duplicate ready event')
            self.ready[account] = self.identifier(event, 'cid')
            return
        if account not in self.ready or self.identifier(event, 'cid') != self.ready[account]:
            raise ValueError('connection identity changed or event preceded authentication')

        if kind == 'joined':
            if event.get('id') != f'join_{account}' or event.get('name') != self.join_name:
                raise ValueError('join command or fence name mismatch')
            fid = self.identifier(event, 'fid')
            if account in self.joined or (self.fid is not None and self.fid != fid):
                raise ValueError('duplicate join or clients joined different fences')
            self.fid = fid
            self.joined[account] = event
        elif kind == 'renamed':
            if not self.concurrent and not self.expect_applied:
                raise ValueError('a rename that had to be refused was applied')
            if event.get('id') != self.request_id(account):
                raise ValueError('rename correlation mismatch')
            if self.identifier(event, 'fid') != self.fid or event.get('name') != self.new_name:
                raise ValueError('rename confirmation names another fence or title')
            # a `renamed` event is a pending request's own answer, and the server always writes that
            # as ACCEPTED; UPDATED is the copy it fans out to the other members
            if event.get('outcome') != OUTCOME_FNAME_OWN or event.get('args_error') != 0:
                raise ValueError('rename confirmation is not the member\'s own copy')
            if not self.concurrent and self.when_client is not None and event.get('when_client') != self.when_client:
                raise ValueError('rename confirmation carries another client stamp')
            if account in self.renamed:
                raise ValueError('duplicate rename confirmation')
            if account in self.refused:
                raise ValueError('a member was both refused and applied')
            if self.concurrent and self.renamed:
                raise ValueError('a second member applied the rename')
            self.renamed[account] = event
            self.when_client = event.get('when_client')
        elif kind == 'fence':
            if not self.concurrent and not self.expect_applied:
                raise ValueError('a refused change was delivered to a member')
            if event.get('op') != FNAME_OPERATION:
                raise ValueError('unexpected unsolicited fence command')
            if self.identifier(event, 'fid') != self.fid or event.get('name') != self.new_name:
                raise ValueError('name change names another fence or title')
            if event.get('outcome') not in OUTCOME_FNAME_APPLIED or event.get('args_error') != 0:
                raise ValueError('name change did not apply')
            if not self.concurrent and self.when_client is not None and event.get('when_client') != self.when_client:
                raise ValueError('name change carries another client stamp')
            if account in self.pushes:
                raise ValueError('duplicate name change delivery')
            if account in self.renamed:
                raise ValueError('name change both pulled and pushed to one member')
            self.pushes[account] = event
        elif kind == 'rename_refused':
            if self.expect_applied and not self.concurrent:
                raise ValueError(f'rename refused (args_error={event.get("args_error")})')
            if event.get('id') != self.request_id(account):
                raise ValueError('refusal correlation mismatch')
            if self.identifier(event, 'fid') != self.fid:
                raise ValueError('refusal names another fence')
            if event.get('name') != self.new_name:
                raise ValueError('refusal names another title')
            if event.get('outcome') != OUTCOME_FNAME_REFUSED:
                raise ValueError('refusal did not carry a rejected outcome')
            # A concurrent proposal from anyone but the owner never reaches the name check: the
            # controller tests ownership first, so it is refused for that instead.
            expected_error = FNAME_ERROR_PERMISSIONS_ADMIN if self.concurrent else FNAME_ERROR_ATTRIBUTE_IDENTICAL
            if event.get('args_error') != expected_error:
                raise ValueError('refusal carries another reason')
            self.identifier(event, 'when_client')
            if account in self.refused:
                raise ValueError('duplicate rename refusal')
            if account in self.renamed:
                raise ValueError('a member was both refused and applied')
            if not self.concurrent and account != self.renamer:
                raise ValueError('refusal came from another member')
            self.refused[account] = event
        elif kind == 'left':
            if account in self.left or self.identifier(event, 'fid') != self.fid:
                raise ValueError('duplicate or mismatched leave')
            if event.get('id') != f'leave_{account}':
                raise ValueError('leave command correlation mismatch')
            self.left.add(account)
        elif kind == 'stopped':
            if account in self.stopped or event.get('id') != f'stop_{account}':
                raise ValueError('duplicate or mismatched stop')
            self.stopped.add(account)
        else:
            raise ValueError(f'unknown CLI event: {kind}')

    def confirmed(self):
        if self.concurrent:
            # the fan-out is the winner's copy to every other member, so the run is confirmed when
            # every request is resolved and every other member has received the applied change
            return (len(self.renamed) == 1 and len(self.refused) == len(self.identities) - 1
                    and len(self.pushes) == len(self.identities) - 1
                    and all((account in self.renamed) != (account in self.refused)
                            for account in self.identities))
        if not self.expect_applied:
            return len(self.refused) == 1
        return len(self.renamed) == 1 and len(self.pushes) == len(self.joined) - 1

    def verify(self):
        if len(self.joined) != len(self.identities) or not self.confirmed():
            raise ValueError('joins or name change confirmations are incomplete')
        if self.concurrent:
            winner = next(iter(self.renamed), None)
            if winner is None or len(self.renamed) != 1:
                raise ValueError('exactly one member must apply the rename')
            if winner != self.renamer:
                raise ValueError('the fence owner must be the one that applies the change')
            if set(self.refused) != set(self.identities) - {winner}:
                raise ValueError('every other member must be refused')
            if set(self.pushes) - (set(self.identities) - {winner}):
                raise ValueError('the member that applied the rename must not be pushed the change')
            return
        if not self.expect_applied:
            if set(self.refused) != {self.renamer}:
                raise ValueError('the refusal was not reported by the member that issued the rename')
            if self.renamed or self.pushes:
                raise ValueError('a refused change was applied or delivered')
            return
        if set(self.renamed) != {self.renamer}:
            raise ValueError('the rename was not confirmed by the member that issued it')

    def summary(self):
        return dict(members=len(self.identities), joined=sorted(self.joined),
                    renamed=[dict(account=account, when_client=event.get('when_client'), outcome=event.get('outcome'))
                             for account, event in self.renamed.items()],
                    pushed=[dict(account=account, when_client=event.get('when_client'), outcome=event.get('outcome'))
                            for account, event in self.pushes.items()],
                    refused=[dict(account=account, args_error=event.get('args_error'),
                                  outcome=event.get('outcome'))
                             for account, event in self.refused.items()],
                    left=sorted(self.left), stopped=sorted(self.stopped))


class FenceCache:
    """Read-only view of the fence backend, through docker exec or a local port."""

    def __init__(self, args):
        self.local = args.local
        self.host = os.environ.get('REDIS_HOST', '127.0.0.1')
        self.cli = os.environ.get('REDIS_CLI', 'redis-cli')
        self.backend = args.redis_fence
        self.port = os.environ.get('FENCE_PORT', '63792')

    def __call__(self, *command):
        prefix = ([self.cli, '-h', self.host, '-p', self.port] if self.local
                  else ['docker', 'exec', self.backend, 'redis-cli'])
        return subprocess.check_output(prefix + ['--raw', *command], text=True, errors='replace', timeout=15).rstrip('\n')

    def ping(self):
        return self('PING') == 'PONG'

    def record(self, fid, *fields):
        return self('HMGET', f'BID:{fid}', *fields).splitlines()

    def dname(self, fid):
        return self('HGET', f'BID:{fid}', 'dname')

    def cname(self, fid):
        return self('HGET', f'BID:{fid}', 'cname')

    def event_counter(self, fid):
        value = self('HGET', f'BID:{fid}', 'event_counter')
        return int(value) if value.isdigit() else 0

    def registry(self, cname, fid):
        return self('ZSCORE', 'FENCES_MAIN', f'{cname}:{fid}')

    def name_token(self, name):
        return unicodedata.normalize('NFD', name.casefold())

    def name_index(self, name):
        token = self.name_token(name)
        return self('ZRANGEBYLEX', 'FENCES_NAMEINDEX', f'[{token}:', f'({token};').splitlines()

    def queued(self, fid, eid):
        """Records still staged for one fence event; a consumed queue is zero, a missing key empty."""
        value = self('LLEN', f'FENCE_OUT_QUEUE:{fid}:{eid}')
        return int(value) if value.isdigit() else 0

    def queued_events(self, fid):
        """Every event id that still has a queue key under this fence, found without KEYS."""
        cursor, events = '0', []
        while True:
            reply = self('SCAN', cursor, 'MATCH', f'FENCE_OUT_QUEUE:{fid}:*', 'COUNT', '1000').splitlines()
            cursor, keys = reply[0], reply[1:]
            for key in keys:
                suffix = key.rsplit(':', 1)[-1]
                if suffix.isdigit():
                    events.append(int(suffix))
            if cursor == '0':
                return events

    def attempts(self, fid, eid):
        """Drain-attempt log entries written for one fence event."""
        return self('LRANGE', f'FENCE_COMMAND_OUT_QUEUE_LOG:{fid}:{eid}', '0', '-1').splitlines()


def run_scenario(kind, args, cache, identities, evidence, report):
    """Drive one rename and assert the clients and the fence cache agree on it."""
    join_name = f'{args.prefix}_{report["tag"]}_{kind}'
    same_name = kind == 'same'
    concurrent = kind == 'concurrent'
    requested = join_name if same_name else f'{join_name}_r'
    accounts = list(identities)[:1 if kind == 'single' else len(identities)]
    renamer = accounts[0]
    rename_id = f'rename_{renamer}'
    ledger = RenameLedger({account: identities[account] for account in accounts}, join_name, rename_id, requested,
                          renamer, expect_applied=not same_name, concurrent=concurrent)
    scenario = dict(kind=kind, fence=join_name, renamed=dict(fid=None, name=requested), checks=[], passed=False)
    report['scenarios'][kind] = scenario

    def check(label, passed):
        scenario['checks'].append(dict(label=label, passed=bool(passed)))
        print(f'{"PASS" if passed else "FAIL"} [{kind}] {label}', flush=True)

    def require(label, passed):
        check(label, passed)
        if not passed:
            raise ValueError(label)

    directory = evidence / kind
    directory.mkdir(mode=0o700)
    cli = Path(args.cli)
    clients = {}
    selector = selectors.DefaultSelector()
    deadline = time.monotonic() + args.timeout

    def send(account, command):
        client = clients[account]
        client['proc'].stdin.write((json.dumps(command) + '\n').encode())
        client['proc'].stdin.flush()
        client['commands'].write(json.dumps(command) + '\n')
        client['commands'].flush()

    def pump(wait=0.1):
        if time.monotonic() >= deadline:
            raise TimeoutError(f'{kind}: rename proof deadline expired')
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
                raise ValueError(f'account {account}: CLI event stream closed before completion '
                                 f'(exit={client["proc"].poll()})')
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
                raise ValueError(f'account {account}: CLI exited before completion '
                                 f'(exit={client["proc"].returncode})')

    def until(predicate):
        while not predicate():
            pump()

    try:
        for account in accounts:
            home = directory / str(account) / 'profile'
            home.mkdir(mode=0o700, parents=True)
            for name in ('ufsrvcli.config', 'ufsrvcli.prefs'):
                shutil.copyfile(args.home_base / str(account) / name, home / name)
                (home / name).chmod(0o600)
            log = (directory / str(account) / 'cli.log').open('wb')
            client_env = dict(os.environ, ASAN_OPTIONS=os.environ.get('ASAN_OPTIONS', 'detect_leaks=0'))
            proc = subprocess.Popen([str(cli), '--headless', '--command-stream', '--home-base', str(home),
                                     '--session-timeout-ms', str(math.ceil(args.timeout * 1000))],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, env=client_env)
            os.set_blocking(proc.stdout.fileno(), False)
            clients[account] = dict(proc=proc, log=log, buffer=b'',
                                    events=(directory / str(account) / 'events.jsonl').open('w'),
                                    commands=(directory / str(account) / 'commands.jsonl').open('w'))
            selector.register(proc.stdout, selectors.EVENT_READ, account)
            until(lambda: account in ledger.ready)
            if account != accounts[-1]:
                end = time.monotonic() + args.launch_spacing
                while time.monotonic() < end:
                    pump(min(0.1, end - time.monotonic()))

        for account in accounts:
            send(account, dict(id=f'join_{account}', op='join', name=join_name))
            until(lambda: account in ledger.joined)
        fid = ledger.fid
        scenario['renamed']['fid'] = str(fid)
        require('all members joined one fence', len(ledger.joined) == len(accounts) and fid is not None)

        before = cache.record(fid, 'dname', 'cname', 'event_counter')
        require('fence record is readable', len(before) == 3)
        require('fence record carries the joined name', before[0] == join_name and before[1].endswith(join_name))
        old_cname = before[1]
        baseline = int(before[2]) if before[2].isdigit() else 0
        index_before = cache.name_index(join_name)
        queued_before = cache.queued_events(fid)
        require('name index carries the joined name', bool(index_before))
        check('canonical registry carries the joined name', cache.registry(old_cname, fid) == '0')

        if same_name:
            send(renamer, dict(id=rename_id, op='rename', fid=str(fid), name=requested))
            until(ledger.confirmed)
            ledger.verify()
            # A rename to the title the fence already carries is refused, and a refusal must leave
            # no trace: no member is told about a change, and the fence record, the registry, the
            # name index and the event counter stay exactly as they were.
            require('the renaming member observed the refusal', renamer in ledger.refused)
            require('no member was told about a change', not ledger.renamed and not ledger.pushes)
            require('the fence record is unchanged by the refusal',
                    cache.record(fid, 'dname', 'cname') == before[:2])
            require('the canonical registry is unchanged by the refusal',
                    cache.registry(old_cname, fid) == '0')
            require('the name index is unchanged by the refusal',
                    cache.name_index(join_name) == index_before)
            require('the refusal did not advance the fence event counter',
                    cache.event_counter(fid) == baseline)
            require('the refusal staged nothing', cache.queued_events(fid) == queued_before)
        else:
            if concurrent:
                # Every member proposes the same title at once. The fence lock serialises the
                # requests, so the first proposal is applied and each later one finds its title
                # already set and is refused.
                for account in accounts:
                    send(account, dict(id=f'rename_{account}', op='rename', fid=str(fid), name=requested))
            else:
                send(renamer, dict(id=rename_id, op='rename', fid=str(fid), name=requested))
            until(ledger.confirmed)
            ledger.verify()

            winner = renamer
            if concurrent:
                winner = next(iter(ledger.renamed))
                scenario['winner'] = winner
                require('exactly one member applied the rename', len(ledger.renamed) == 1)
                require('the fence owner is the one that applied it', winner == renamer)
                require('every other member was refused',
                        set(ledger.refused) == set(accounts) - {winner})
            else:
                require(f'the renaming member observed the change ({kind})', renamer in ledger.renamed)

            require('every other member observed the change',
                    sorted(ledger.pushes) == sorted(account for account in accounts if account != winner))

            record = cache.record(fid, 'dname', 'cname', 'event_counter')
            cname = record[1]
            check('fence record carries the new name', record[0] == requested)
            check('canonical name follows the rename', cname.endswith(requested) and
                  cname[:-len(requested)] == old_cname[:-len(join_name)])
            check('canonical registry replaced the old entry', cache.registry(old_cname, fid) == '' and
                  cache.registry(cname, fid) == '0')
            check('name index replaced the old token', not cache.name_index(join_name) and
                  any(f':{fid}:' in member for member in cache.name_index(requested)))
            check('the rename advanced the fence event counter by one',
                  record[2].isdigit() and int(record[2]) == baseline + 1)

            # The event the rename was registered under is also the staged queue key: the fan-out
            # must have been consumed by the drain, and the drain must have logged its attempt.
            eid = int(record[2]) if record[2].isdigit() else 0
            if len(accounts) > 1:
                # the fan-out targets every member but the originator, so the record covers them all
                check('the staged rename record was consumed', cache.queued(fid, eid) == 0)

                # The drain writes its attempt after the delivery the clients just reported, so wait
                # for it for a bounded moment. A server that does not stage fence commands never
                # writes one: say so rather than waiting out the scenario deadline.
                attempts = cache.attempts(fid, eid)
                if not attempts:
                    grace = time.monotonic() + STAGED_GRACE_SECONDS
                    while time.monotonic() < grace and not attempts:
                        pump(min(0.1, max(0, grace - time.monotonic())))
                        attempts = cache.attempts(fid, eid)
                if not attempts and not cache.queued_events(fid):
                    print(f'NOTE [{kind}] no staged fence-command record exists for event {eid}: '
                          'this server does not stage fence commands, so the queue and attempt-log checks fail',
                          flush=True)
                check('the drain logged the delivered attempt',
                      any(line.startswith('0:outcome:*') for line in attempts))
                check('no staged fence command is left for this fence',
                      all(cache.queued(fid, key_eid) == 0 for key_eid in cache.queued_events(fid)))
            else:
                # the only member is the originator, whose copy is written immediately: nothing to queue
                check('a single-member rename queues nothing', not cache.queued_events(fid))

        settle = time.monotonic() + args.settle_ms / 1000
        while time.monotonic() < settle:
            pump(min(0.1, settle - time.monotonic()))
        ledger.verify()
        if same_name:
            check('no member received a second answer', len(ledger.refused) == 1 and not ledger.renamed)
            check('the fence still carries the joined title', cache.dname(fid) == join_name)
            check('the fence event counter is still unchanged', cache.event_counter(fid) == baseline)
        else:
            check('no member received the change twice', len(ledger.renamed) == 1 and
                  len(ledger.pushes) == len(accounts) - 1)

        for account in accounts:
            send(account, dict(id=f'leave_{account}', op='leave', fid=str(fid)))
            until(lambda: account in ledger.left)
        for account in accounts:
            send(account, dict(id=f'stop_{account}', op='stop'))
        until(lambda: len(ledger.stopped) == len(accounts))
        for account, client in clients.items():
            code = client['proc'].wait(timeout=max(0.1, deadline - time.monotonic()))
            client['exit'] = code
            require(f'account {account} exited 0 (exit={code}, log=cli.log)', code == 0)

        if same_name:
            check('the fence is unchanged after the clients exited',
                  cache.record(fid, 'dname', 'cname') == before[:2])
            check('the canonical registry still names the fence after exit',
                  cache.registry(old_cname, fid) == '0')
            check('the name index still names the fence after exit', cache.name_index(join_name) == index_before)
            # the teardown itself registers a leave event per member, so the counter is judged
            # before the leaves, not here
            check('the refusal left no queue behind', cache.queued_events(fid) == queued_before)
        else:
            record = cache.record(fid, 'dname', 'cname')
            check('the rename is still in Redis after the clients exited',
                  len(record) == 2 and record[0] == requested and record[1] == cname)
            check('the canonical registry still names the fence after exit', cache.registry(cname, fid) == '0')
            check('the name index still names the fence after exit',
                  any(f':{fid}:' in member for member in cache.name_index(requested)))
            check('the rename queue is still empty after the clients exited', cache.queued(fid, eid) == 0)

        scenario['passed'] = all(item['passed'] for item in scenario['checks'])
        require(f'{kind}: every rename invariant holds', scenario['passed'])
    finally:
        scenario['events'] = ledger.summary()
        scenario['cli_exits'] = {account: client.get('exit', client['proc'].poll())
                                 for account, client in clients.items()}
        for client in clients.values():
            proc = client['proc']
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            for name in ('log', 'events', 'commands'):
                client[name].close()
            proc.stdin.close()
            proc.stdout.close()
        selector.close()
    return scenario['passed']


def scenario_list(args):
    """The scenarios this invocation runs."""
    if args.scenario == 'both':
        return ['single', 'multi']
    if args.scenario == 'all':
        return ['single', 'multi', 'same', 'concurrent']
    return [args.scenario]


def describe(args):
    """Print what this invocation will do and what success looks like, for the chosen switches."""
    scenarios = scenario_list(args)
    members = {'single': 1, 'multi': args.agents, 'same': args.agents, 'concurrent': args.agents}
    fence = f'{args.prefix}_<run>_<scenario>'
    renamed = f'{fence}_r'
    where = ('local port ' + os.environ.get('FENCE_PORT', '63792')) if args.local \
        else f'`docker exec {args.redis_fence} redis-cli --raw`'

    print('Fence rename (FNAME) proof: what this invocation checks')
    print()
    print(f'  scenarios        : {args.scenario} ({", ".join(scenarios)})')
    for kind in scenarios:
        print(f'  {kind:<16} : {members[kind]} member(s) per fence')
    print(f'  fence names      : {fence}; the applied scenarios rename it to {renamed}, `same` to itself')
    print('                     (the <run> tag is generated at run time)')
    print(f'  profiles         : {args.home_base}'
          f', accounts {args.account}..{args.account + max(members.values()) - 1}')
    print(f'  client           : {args.cli}')
    print(f'  fence backend    : {where}')
    print(f'  evidence         : {args.evidence_dir}/fence_rename_<run>')
    print(f'  limits           : {args.timeout:g}s per scenario deadline, {args.settle_ms}ms settle window, '
          f'{args.launch_spacing:g}s launch spacing')
    print()
    print('Each scenario opens one persistent `--command-stream` CLI per member, joins the fence,')
    print('renames it from the first member, and requires the change to be observed before tearing down.')
    print('The `same` scenario instead renames the fence to the title it already has, and requires the')
    print('refusal to be observed and to leave the fence untouched. The `concurrent` scenario has every')
    print('member propose the same new title at once, and requires exactly one of them to win.')
    print()
    print('Success, per scenario:')
    if 'single' in scenarios:
        print('  single: the renaming member emits exactly one `renamed` event carrying the new title,')
        print('          outcome ACCEPTED and its own echoed client stamp.')
    if 'multi' in scenarios:
        print(f'  multi : the renaming member emits one `renamed` (ACCEPTED) event; each of the other')
        print(f'          {args.agents - 1} member(s) emits exactly one `fence` event with op=4 (FNAME),')
        print('          the same fence id, title, outcome UPDATED and the same client stamp.')
    if 'concurrent' in scenarios:
        print('  concurrent: every member proposes the same new title at once; the fence owner is the')
        print('          only one that may rename, so it emits `renamed` (ACCEPTED) and each other member')
        print(f'          emits `rename_refused` with args_error {FNAME_ERROR_PERMISSIONS_ADMIN} (PERMISSIONS_ADMIN), and')
        print('          every member sees the one applied change.')
    if 'same' in scenarios:
        print('  same  : the renaming member emits exactly one `rename_refused` with outcome REJECTED,')
        print(f'          args_error {FNAME_ERROR_ATTRIBUTE_IDENTICAL} (ATTRIBUTE_IDENTICAL) and the title it tried to')
        print('          set; every other member emits nothing, and no member ever emits `renamed`.')
    print('  every member exits 0 after an explicit `stop`, and no member sees a duplicate or a')
    print('  change that names another fence, another title or a refused outcome.')
    print()
    print('Success, per fence, read from the fence backend twice (after the confirmations and again')
    print('after every client exited):')
    print('  BID:<fid> dname                 the new title')
    print('  BID:<fid> cname                 the old canonical prefix with the new title appended')
    print('  FENCES_MAIN                      old <cname>:<fid> gone, new one at score 0')
    print('  FENCES_NAMEINDEX                 no member under the old folded name, one under the new')
    print('  BID:<fid> event_counter          advanced by exactly one for the rename')
    print('  FENCE_OUT_QUEUE:<fid>:<eid>      consumed by the drain (LLEN 0), and still empty after exit')
    print('  FENCE_COMMAND_OUT_QUEUE_LOG:*    at least one `0:outcome:*` entry: a delivered attempt')
    print('  SCAN FENCE_OUT_QUEUE:<fid>:*     no other event of this fence still has a queue key')
    print()
    if 'same' in scenarios:
        print('For `same` the same keys are read twice and required to be unchanged, together with the')
        print('event counter and the set of queued events: a refusal may not mutate, fan out or stage.')
    print()
    print('Failure includes: a missing or duplicated confirmation, a wrong fence/title/stamp/outcome,')
    print('a member that never confirms, a non-zero CLI exit, a staged record the drain did not take,')
    print('or a fence record that does not carry the new name after teardown, or a refusal that was')
    print('applied, delivered to a member, or that changed the fence.')
    print()
    print('Nothing was run; drop --invariants to execute.')


def main(argv=None):
    parser = argparse.ArgumentParser(description='Fence rename (FNAME) delivery and persistence proof')
    parser.add_argument('--home-base', type=Path, default=Path.home() / 'ufsrvcli/test_agents',
                        help='directory of numbered account profiles (default: ~/ufsrvcli/test_agents)')
    parser.add_argument('--account', type=int, default=1, help='first account directory')
    parser.add_argument('--agents', type=int, default=3, help='members in the multi-member scenario')
    parser.add_argument('--scenario',
                        choices=('single', 'multi', 'same', 'concurrent', 'both', 'all'), default='both')
    parser.add_argument('--timeout', type=float, default=120, help='per-scenario deadline in seconds')
    parser.add_argument('--settle-ms', type=int, default=500, help='observation window after the change')
    parser.add_argument('--launch-spacing', type=float, default=1.2, help='seconds between CLI starts')
    parser.add_argument('--prefix', default='fnameproof')
    parser.add_argument('--cli', default=str(Path(__file__).resolve().parents[3] / 'ufsrvcli/build/dev/src/ufsrvcli'))
    parser.add_argument('--local', action='store_true', help='reach Redis by port instead of docker exec')
    parser.add_argument('--redis-fence', default=os.environ.get('REDIS_FENCE', 'redis-fence'))
    parser.add_argument('--print-invariants', action='store_true',
                        help='print the run expectation once, then execute')
    parser.add_argument('--invariants', action='store_true',
                        help='describe what this invocation checks and what success looks like, then exit')
    parser.add_argument('--evidence-dir', type=Path, default=Path('/tmp/concurrency_correctness_proofs'))
    args = parser.parse_args(argv)
    members = 1 if args.scenario == 'single' else args.agents
    if args.account < 1 or members < 1 or (args.scenario != 'single' and args.agents < 2) \
            or not math.isfinite(args.timeout) or not 0 < args.timeout <= 2147483:
        parser.error('positive account required, at least two agents for the multi scenario, '
                     'and a bounded timeout')
    if args.settle_ms < 1 or not math.isfinite(args.launch_spacing) or args.launch_spacing < 0:
        parser.error('settle-ms must be positive and launch spacing finite/nonnegative')

    if args.invariants:
        describe(args)
        return 0
    args.home_base = args.home_base.resolve()
    args.cli = str(Path(args.cli).resolve())
    accounts = list(range(args.account, args.account + members))
    identities = {}
    cookies = set()
    for account in accounts:
        prefs = args.home_base / str(account) / 'ufsrvcli.prefs'
        if not prefs.is_file():
            parser.error(f'missing profile: {prefs}')
        text = prefs.read_text()
        uid = int.from_bytes(uid_bytes(lua_value(text, 'ufsrvuid'))[8:], 'little')
        cookie = lua_value(text, 'cookie')
        if not uid or uid in identities.values() or not cookie or cookie in cookies:
            parser.error(f'account {account}: distinct nonempty UIDs and cookies required')
        identities[account] = uid
        cookies.add(cookie)

    os.umask(0o077)
    args.evidence_dir.mkdir(parents=True, exist_ok=True)
    evidence = Path(tempfile.mkdtemp(prefix='fence_rename_', dir=args.evidence_dir))
    report = dict(passed=False, started=time.time(), agents=args.agents, tag=evidence.name.split('_')[-1],
                  verification='decoded CLI events for delivery; fence Redis record, registry and name index for persistence')
    print(f'Evidence directory: {evidence}', flush=True)
    if args.print_invariants:
        print('Expected: the renaming member observes exactly one applied FNAME confirmation (for `same`, '
              'exactly one refusal; for `concurrent`, the owner is applied and the rest refused), every '
              'other member observes the change once (nothing, for `same`), and Redis carries the new name '
              'in BID, FENCES_MAIN and FENCES_NAMEINDEX both when the change is delivered and after every '
              'CLI exited - or stays unchanged, for `same`.', flush=True)

    cache = FenceCache(args)
    try:
        if not cache.ping():
            raise ValueError(f'fence Redis unavailable: {args.redis_fence}')
        if not os.access(args.cli, os.X_OK):
            raise ValueError(f'CLI is not executable: {args.cli}')
        scenarios = scenario_list(args)
        report['scenarios'] = {}
        results = [run_scenario(kind, args, cache, identities, evidence, report) for kind in scenarios]
        report['passed'] = all(results)
    except (Exception, KeyboardInterrupt) as error:
        report['error'] = str(error) or 'interrupted'
        report['passed'] = False
        print(f'FAIL: {report["error"]}', flush=True)
    report['finished'] = time.time()
    (evidence / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    for kind, scenario in report.get('scenarios', {}).items():
        checks = scenario.get('checks', [])
        print(f'{kind}: fence={scenario["fence"]} fid={scenario["renamed"]["fid"]} '
              f'checks={sum(item["passed"] for item in checks)}/{len(checks)}')
    print(f'{"PASS" if report["passed"] else "FAIL"}: report={evidence / "report.json"}')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
