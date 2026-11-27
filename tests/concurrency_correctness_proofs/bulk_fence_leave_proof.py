import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
from threading import Event, Lock
import time
import unicodedata


BASE_FENCE = 1 << 4
USER_FENCE = 1 << 7
STICKY_FENCE = 1 << 11
FIELDS = ('id', 'type', 'event_counter', 'cname', 'dname', 'uid', 'lng', 'lat')


def arguments(argv):
    parser = argparse.ArgumentParser(description='Leave fences and independently report confirmations and Redis mutations.')
    parser.add_argument('--home-base', required=True, type=Path)
    parser.add_argument('--account', type=int, default=1)
    parser.add_argument('--workers', type=int, default=1, help='concurrent workers for the selected account; capped at selected fences')
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument('--list-fences', action='store_true')
    selection.add_argument('--all-fences', action='store_true')
    selection.add_argument('--fences', help='comma-separated numeric fence IDs for one client')
    parser.add_argument('--local', action='store_true')
    parser.add_argument('--cli', default=os.environ.get('CLI', '/home/devops/repos/1.ufsrv/ufsrvcli/build/dev/src/ufsrvcli'))
    parser.add_argument('--redis-session', default=os.environ.get('REDIS_SESSION', 'redis-session'))
    parser.add_argument('--redis-fence', default=os.environ.get('REDIS_FENCE', 'redis-fence'))
    parser.add_argument('--evidence-dir', type=Path, default=os.environ.get('EVIDENCE_DIR', '/tmp/concurrency_correctness_proofs'))
    parser.add_argument('--container', default=os.environ.get('CONTAINER', 'ufsrvwebsock'))
    parser.add_argument('--api-container', default=os.environ.get('APIC', 'ufsrvapi'))
    parser.add_argument('--log', type=Path, default=os.environ.get('LOG', '/var/log/ufsrvwebsock.log'))
    parser.add_argument('--api-log', type=Path, default=os.environ.get('APILOG', '/var/log/ufsrvapi.log'))
    parser.add_argument('--timeout', type=float, default=90, help='deadline for each CLI invocation, seconds')
    args = parser.parse_args(argv)
    if args.account < 1 or args.workers < 1 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('account, workers and timeout must be finite and positive')
    args.requested = []
    if args.fences is not None:
        args.requested = [value.removeprefix('#') for value in args.fences.split(',')]
        if any(not re.fullmatch(r'[1-9][0-9]*', fid) or int(fid) > 2**64 - 1 for fid in args.requested):
            parser.error('--fences requires comma-separated positive numeric fence IDs')
        if len(set(args.requested)) != len(args.requested):
            parser.error('--fences contains duplicate IDs')
    return args


def redis_csv(line):
    values = []
    position = 0
    escapes = {'n': 10, 'r': 13, 't': 9, 'b': 8, 'a': 7, '\\': 92, '"': 34}
    while position < len(line):
        if line[position] == '"':
            position += 1
            data = bytearray()
            while position < len(line) and line[position] != '"':
                character = line[position]
                position += 1
                if character != '\\':
                    data.extend(character.encode('utf-8'))
                    continue
                if position >= len(line):
                    raise ValueError('truncated Redis CSV escape')
                escape = line[position]
                position += 1
                if escape == 'x':
                    code = line[position:position+2]
                    if not re.fullmatch(r'[0-9a-fA-F]{2}', code):
                        raise ValueError('invalid Redis CSV hex escape')
                    data.append(int(code, 16))
                    position += 2
                elif escape in escapes:
                    data.append(escapes[escape])
                else:
                    raise ValueError(f'invalid Redis CSV escape: {escape}')
            if position >= len(line):
                raise ValueError('unterminated Redis CSV string')
            position += 1
            value = data.decode('utf-8')
        else:
            end = line.find(',', position)
            end = len(line) if end < 0 else end
            token = line[position:end]
            if token == 'NULL':
                value = None
            elif re.fullmatch(r'-?[0-9]+', token):
                value = int(token)
            else:
                raise ValueError(f'Redis error or invalid CSV value: {token}')
            position = end
        values.append(value)
        if position < len(line):
            if line[position] != ',' or position+1 == len(line):
                raise ValueError('invalid Redis CSV separator')
            position += 1
    return values


def identity(text):
    uids = set(re.findall(r'X-UFSRVUID: ([1-9][0-9]*)[ \t]*$', text, re.MULTILINE))
    if len(uids) != 1:
        raise ValueError('could not identify one numeric X-UFSRVUID')
    return uids.pop()


def user_fences(values):
    if not isinstance(values, list):
        raise ValueError('UF index did not return an array')
    result = []
    for member in values:
        match = re.fullmatch(r'([1-9][0-9]*):[0-9]+', member or '')
        if not match:
            raise ValueError(f'unexpected UF member: {member!r}')
        if match[1] not in result:
            result.append(match[1])
    return result


class LeaveProof:
    def __init__(self, args, workdir=None):
        self.args = args
        self.workdir = workdir
        self.cli = Path(args.cli).resolve()
        self.stop = Event()
        self.lock = Lock()
        self.clients = []
        self.before = {}
        self.counts = Counter()
        self.geo_before = {}
        self.logs = {}
        self.report = dict(status='FAIL', exit_code=2, phase='preflight', started_at=datetime.now().astimezone().isoformat(),
                           finished_at=None, errors=[], batches=[], fences=[], summary={}, evidence={}, correlation={})
        self.transcript = (workdir / 'run.log').open('w', buffering=1) if workdir else None

    def emit(self, text):
        with self.lock:
            if self.transcript:
                self.transcript.write(text + '\n')
            try:
                print(text, flush=True)
            except BrokenPipeError:
                if not self.transcript:
                    raise

    def error(self, text):
        with self.lock:
            self.report['errors'].append(str(text))
        self.emit(f'FAIL: {text}')

    def redis(self, backend, commands):
        if not commands:
            return []
        if self.args.local:
            port = os.environ.get('SESSION_PORT', '63791') if backend == 'session' else os.environ.get('FENCE_PORT', '63792')
            command = [os.environ.get('REDIS_CLI', 'redis-cli'), '-h', os.environ.get('REDIS_HOST', '127.0.0.1'), '-p', port]
        else:
            container = self.args.redis_session if backend == 'session' else self.args.redis_fence
            command = ['docker', 'exec', '-i', container, 'redis-cli']
        data = ''.join(' '.join(json.dumps(str(value), ensure_ascii=False) for value in row)+'\n' for row in commands)
        result = subprocess.run(command+['--csv'], input=data, text=True, capture_output=True, timeout=10)
        if result.returncode:
            raise ValueError(f'{backend} Redis CLI exit {result.returncode}: {result.stderr.strip() or result.stdout.strip()}')
        lines = result.stdout.splitlines()
        if len(lines) != len(commands):
            raise ValueError(f'{backend} Redis: expected {len(commands)} replies, received {len(lines)}')
        values = []
        for request, line in zip(commands, lines):
            cells = redis_csv(line)
            if request[0] in ('ZRANGE', 'HMGET'):
                values.append(cells)
            elif len(cells) == 1:
                values.append(cells[0])
            else:
                raise ValueError(f'{backend} Redis: invalid scalar response for {request[0]}')
        return values

    def cli_run(self, home, log, options=()):
        command = [str(self.cli), '--headless', '--home-base', str(home), *options, '--']
        if self.workdir:
            with self.lock, (log.parent / 'commands.txt').open('a') as output:
                output.write(shlex.join(command)+'\n')
        started = time.monotonic()
        with log.open('w') as output:
            process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                       env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'), start_new_session=True)
            status = 'EXITED'
            try:
                while process.poll() is None:
                    if self.stop.is_set():
                        status = 'INTERRUPTED'
                        break
                    if time.monotonic()-started >= self.args.timeout:
                        status = 'TIMEOUT'
                        break
                    try:
                        process.wait(timeout=.1)
                    except subprocess.TimeoutExpired:
                        pass
            finally:
                if process.poll() is None:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait()
        return process.returncode, status

    def profile(self, account):
        home = (self.args.home_base / str(account)).resolve()
        if not self.cli.is_file() or not os.access(self.cli, os.X_OK):
            raise ValueError(f'CLI is not executable: {self.cli}')
        for filename in ('ufsrvcli.config', 'ufsrvcli.prefs'):
            if not (home / filename).is_file() or not os.access(home / filename, os.R_OK):
                raise ValueError(f'unreadable client file: {home / filename}')
        return home

    def log_read(self, container, path, baseline=None):
        if self.args.local:
            with path.open('rb') as stream:
                stat = os.fstat(stream.fileno())
                if baseline:
                    if stat.st_ino != baseline['inode'] or stat.st_size < baseline['offset']:
                        raise ValueError(f'log rotated or truncated during run: {path}')
                    stream.seek(baseline['offset'])
                    return stream.read().decode('utf-8', errors='replace')
                return dict(inode=stat.st_ino, offset=stat.st_size)
        quoted = shlex.quote(str(path))
        if baseline:
            script = f'stat -c "%i %s" {quoted}; tail -c +{baseline["offset"]+1} {quoted}'
        else:
            script = f'stat -c "%i %s" {quoted}'
        result = subprocess.run(['docker', 'exec', container, 'sh', '-c', script], capture_output=True,
                                text=True, errors='replace', timeout=10)
        if result.returncode:
            raise ValueError(f'cannot read {container}:{path}: {result.stderr.strip()}')
        header, _, text = result.stdout.partition('\n')
        inode, size = map(int, header.split())
        if baseline:
            if inode != baseline['inode'] or size < baseline['offset']:
                raise ValueError(f'log rotated or truncated during run: {container}:{path}')
            return text
        return dict(inode=inode, offset=size)

    def baseline_logs(self):
        for label, container, path in (('api', self.args.api_container, self.args.api_log),
                                       ('websock', self.args.container, self.args.log)):
            self.logs[label] = (container, path, self.log_read(container, path))
        self.report['evidence']['log_baselines'] = {label: entry[2] for label, entry in self.logs.items()}

    def capture_logs(self):
        for label, (container, path, baseline) in self.logs.items():
            try:
                text = self.log_read(container, path, baseline)
                destination = self.workdir / f'evidence_{label}.log'
                destination.write_text(text)
                self.report['evidence'][label] = str(destination)
            except Exception as exc:
                self.error(f'capture {label} log: {exc}')
        api_path = self.workdir / 'evidence_api.log'
        ws_path = self.workdir / 'evidence_websock.log'
        api = api_path.read_text(errors='replace') if api_path.exists() else ''
        ws = ws_path.read_text(errors='replace') if ws_path.exists() else ''
        published = list(dict.fromkeys(re.findall(r"_FenceCommandIntraMarshal .*reqid:'(\d+)'", api)))
        processed = list(dict.fromkeys(re.findall(r"reqid_processed:'(\d+)'", ws)))
        self.report['correlation'] = dict(scope='server log window; may include unrelated requests',
                                          api_published_reqids=published, websock_processed_reqids=processed,
                                          published_not_observed_processed=sorted(set(published)-set(processed), key=int))

    def fence_state(self, fids, required):
        values = self.redis('fence', [['HMGET', f'BID:{fid}', *FIELDS] for fid in fids])
        result = {}
        for fid, record in zip(fids, values):
            if not isinstance(record, list) or len(record) != len(FIELDS):
                raise ValueError(f'invalid BID:{fid} reply')
            if all(value is None for value in record) and not required:
                result[fid] = None
                continue
            data = dict(zip(FIELDS, record))
            if data['id'] != fid or not str(data['type']).isdecimal() or not str(data['event_counter']).isdecimal():
                raise ValueError(f'missing or invalid BID:{fid} identity, type, or event counter')
            if not data['cname'] or not data['dname'] or not str(data['uid']).isdecimal():
                raise ValueError(f'missing BID:{fid} name or creator')
            if not all(math.isfinite(float(data[field])) for field in ('lng', 'lat')):
                raise ValueError(f'invalid BID:{fid} coordinates')
            data['type'], data['event_counter'] = int(data['type']), int(data['event_counter'])
            result[fid] = data
        return result

    def snapshot(self, filename, state):
        rows = []
        for fid, record in state.items():
            rows.append(f'{fid}\t{record["type"]}\t{record["event_counter"]}\n' if record else f'{fid}\tDELETED\tUNAVAILABLE\n')
        (self.workdir / filename).write_text(''.join(rows))

    def preflight(self):
        self.baseline_logs()
        seen = set()
        for account in [self.args.account]:
            home = self.profile(account)
            out = self.workdir / f'cli_{account}'
            out.mkdir()
            log = out / 'identity.log'
            rc, status = self.cli_run(home, log)
            if rc or status != 'EXITED':
                raise ValueError(f'identity handshake {status}, exit={rc}: {log}')
            uid = identity(log.read_text(errors='replace'))
            if uid in seen:
                raise ValueError(f'multiple profiles resolve to user {uid}')
            seen.add(uid)
            current = user_fences(self.redis('session', [['ZRANGE', f'UF:{uid}', 0, -1]])[0])
            fids = current if self.args.all_fences else self.args.requested
            rows = [dict(uid=uid, fid=fid, posted=False, acknowledged=False, confirmed=False, applied=None,
                         mutation_ok=False, event_status='NOT_VERIFIED', error=None) for fid in fids]
            self.report['fences'].extend(rows)
            self.clients.append(dict(home=home, out=out, uid=uid, fids=fids, rows=rows))
            scores = self.redis('session', [['ZSCORE', f'MEMBER_USERS_FOR_FENCE:{fid}', uid] for fid in fids])
            for fid, score in zip(fids, scores):
                if fid not in current or score is None or not re.fullmatch(r'-?[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?', str(score)):
                    raise ValueError(f'uid={uid} fid={fid} is missing from a membership index')
            (out / 'expected.txt').write_text(''.join(f'{uid}\t{fid}\n' for fid in fids))
            self.emit(f'Client {account}, uid={uid}, selected {len(fids)} fences')
            self.stop.wait(1.1)
            if self.stop.is_set():
                raise InterruptedError('run interrupted during preflight')
        selected = self.clients.pop()
        count = min(self.args.workers, len(selected['fids']))
        self.report['workers'] = dict(requested=self.args.workers, launched=count)
        for worker in range(count):
            out = self.workdir / f'worker_{worker+1}'
            out.mkdir(mode=0o700)
            home = out / str(self.args.account)
            home.mkdir(mode=0o700)
            for filename in ('ufsrvcli.config', 'ufsrvcli.prefs'):
                shutil.copyfile(selected['home'] / filename, home / filename)
                (home / filename).chmod(0o600)
            rows = selected['rows'][worker::count]
            self.clients.append(dict(home=home, out=out, uid=selected['uid'],
                                     fids=[row['fid'] for row in rows], rows=rows))
        self.counts = Counter(row['fid'] for row in self.report['fences'])
        self.before = self.fence_state(list(self.counts), required=True)
        member_counts = self.redis('session', [['ZCARD', f'MEMBER_USERS_FOR_FENCE:{fid}'] for fid in self.counts])
        for fid, count in zip(self.counts, member_counts):
            if not isinstance(count, int) or count < self.counts[fid]:
                raise ValueError(f'invalid initial member count for fence {fid}: {count}')
            self.before[fid]['members'] = count
        for client in self.clients:
            if any(self.before[fid]['type'] & BASE_FENCE for fid in client['fids']):
                current, past = self.redis('session', [['HGET', f'UID:{client["uid"]}', field] for field in ('geofid_current', 'geofid_past')])
                if current in client['fids'] and self.before[current]['type'] & BASE_FENCE:
                    self.geo_before[client['uid']] = past or '0'
        self.snapshot('fence_events_before.tsv', self.before)
        self.report['before'] = self.before

    def batch_evidence(self, client, batch, log, rc, status, number):
        text = log.read_text(errors='replace') if log.exists() else ''
        posted = re.findall(r'group join: POST \S+ \(fname=#([0-9]+)\)', text)
        acknowledged = re.findall(r'group join: reply for #([0-9]+):', text)
        left_indexes = set()
        for index, header in re.findall(r'group join: StateSync\[(\d+)\] ([^\n]*)', text):
            fields = re.findall(r'\b(command|args|args_error|args_error_client)=(\d+)(?=\s|$)', header)
            values = dict(fields)
            if (len(fields) == len(values) and values.get('command') == '1' and values.get('args') == '2'
                    and values.get('args_error', '0') == '0' and values.get('args_error_client', '0') == '0'):
                left_indexes.add(index)
        confirmed = [fid for index, fid in re.findall(r'group join: group\[(\d+)\] fid=(\d+) fname=', text)
                     if index in left_indexes]
        try:
            correct_uid = identity(text) == client['uid']
        except ValueError:
            correct_uid = False
        valid = (status == 'EXITED' and rc == 0 and correct_uid and Counter(posted) == Counter(batch)
                 and Counter(acknowledged) == Counter(batch) and Counter(confirmed) == Counter(batch))
        record = dict(uid=client['uid'], number=number, requested=len(batch), fids=batch, posted=len(posted),
                      acknowledged=len(acknowledged), confirmed=len(confirmed), exit_code=rc,
                      status='PASS' if valid else status if status != 'EXITED' else 'FAIL', log=str(log))
        with self.lock:
            self.report['batches'].append(record)
            for row in client['rows']:
                if row['fid'] in batch:
                    row.update(posted=row['fid'] in posted, acknowledged=row['fid'] in acknowledged,
                               confirmed=correct_uid and confirmed.count(row['fid']) == 1)
        self.emit(f'Batch uid={client["uid"]} #{number}: {record["status"]}; requested={len(batch)} '
                  f'posted={len(posted)} acknowledged={len(acknowledged)} confirmed={len(confirmed)}; checking Redis')
        return valid

    def membership_results(self, client, selected):
        current = user_fences(self.redis('session', [['ZRANGE', f'UF:{client["uid"]}', 0, -1]])[0])
        scores = self.redis('session', [['ZSCORE', f'MEMBER_USERS_FOR_FENCE:{row["fid"]}', client['uid']] for row in selected])
        for row, score in zip(selected, scores):
            row['uf_absent'] = row['fid'] not in current
            row['reverse_absent'] = score is None
            row['applied'] = row['uf_absent'] and row['reverse_absent']

    def leave(self, client):
        for offset in range(0, len(client['fids']), 16):
            if self.stop.is_set():
                break
            batch = client['fids'][offset:offset+16]
            number = offset//16+1
            log = client['out'] / f'leave_{number}.log'
            self.emit(f'Client uid={client["uid"]}, sending leave batch {number} ({len(batch)} fences); '
                      f'CLI deadline={self.args.timeout:g}s; log={log}')
            rc, status = None, 'ERROR'
            try:
                option = '--unfacd-group-leave='+','.join('#'+fid for fid in batch)
                rc, status = self.cli_run(client['home'], log, [option])
            except Exception as exc:
                self.error(f'uid={client["uid"]} batch {number}: {exc}')
            valid = self.batch_evidence(client, batch, log, rc, status, number)
            selected = client['rows'][offset:offset+16]
            try:
                self.membership_results(client, selected)
                self.emit(f'Batch uid={client["uid"]} #{number}: Redis applied={sum(row["applied"] for row in selected)}/{len(batch)}')
            except Exception as exc:
                self.error(f'uid={client["uid"]} batch {number} Redis verification: {exc}')
                valid = False
            if not valid:
                break

    def deletion_commands(self, fid, record):
        cname = record['cname']
        parts = cname.split(':', 4)
        if len(parts) != 5:
            raise ValueError(f'invalid canonical fence name: {cname}')
        components = {unicodedata.normalize('NFD', name.casefold()) for name in (*parts[:3], record['dname']) if name}
        if parts[3]:
            components.add(record['uid'])
        coords = f'{float(record["lng"]):.6f}:{float(record["lat"]):.6f}'
        return [['ZSCORE', 'FENCES_MAIN', f'{cname}:{fid}'], ['ZSCORE', 'FENCES_GEO', f'{fid}:2:'],
                *[['ZSCORE', 'FENCES_NAMEINDEX', f'{name}:{fid}:{cname}:{coords}'] for name in sorted(components)]]

    def verify(self):
        self.report['phase'] = 'verification'
        for client in self.clients:
            try:
                self.membership_results(client, client['rows'])
            except Exception as exc:
                self.error(f'uid={client["uid"]} final membership verification: {exc}')
        after = {}
        for fid, before in self.before.items():
            try:
                record = self.fence_state([fid], required=False)[fid]
                after[fid] = record
                expected = before['event_counter'] + self.counts[fid] * (2 if before['type'] & BASE_FENCE else 1)
                rows = [row for row in self.report['fences'] if row['fid'] == fid]
                if record is not None:
                    ok = record['type'] == before['type'] and record['event_counter'] == expected
                    for row in rows:
                        row.update(event_status='PASS' if ok else 'FAIL', event_before=before['event_counter'],
                                   event_expected=expected, event_after=record['event_counter'], mutation_ok=bool(row['applied'] and ok))
                else:
                    eligible = bool(before['type'] & USER_FENCE and not before['type'] & (BASE_FENCE | STICKY_FENCE)
                                    and before['members'] == self.counts[fid] and all(row['applied'] for row in rows))
                    count = self.redis('session', [['ZCARD', f'MEMBER_USERS_FOR_FENCE:{fid}']])[0]
                    indexes = self.redis('fence', self.deletion_commands(fid, before))
                    deleted = eligible and count == 0 and all(value is None for value in indexes)
                    for row in rows:
                        row.update(event_status='UNAVAILABLE_DELETED', deletion_ok=deleted, mutation_ok=bool(deleted),
                                   event_before=before['event_counter'], event_expected=expected, event_after=None)
                    if not deleted:
                        self.error(f'BID:{fid} disappeared without verified orphan cleanup')
            except Exception as exc:
                self.error(f'fid={fid} final verification: {exc}')
                for row in self.report['fences']:
                    if row['fid'] == fid:
                        row.update(mutation_ok=False, error=str(exc))
        self.snapshot('fence_events_after.tsv', after)
        self.report['after'] = after
        for uid, past in self.geo_before.items():
            try:
                values = self.redis('session', [['HGET', f'UID:{uid}', field] for field in ('geofid_current', 'geofid_past')])
                if values != ['0', past]:
                    self.error(f'UID:{uid} geofids={values}, expected {["0", past]}')
            except Exception as exc:
                self.error(f'UID:{uid} geofence verification: {exc}')

    def finalize(self, exit_code):
        workers = self.report.get('workers', dict(requested=self.args.workers, launched=0))
        self.emit(f'Workers: requested={workers["requested"]} launched={workers["launched"]}')
        if workers['launched'] < workers['requested']:
            self.emit('Worker count reduced to the number of selected operations (or preflight did not complete).')
        rows = self.report['fences']
        summary = dict(selected=len(rows), posted=sum(row['posted'] for row in rows),
                       acknowledged=sum(row['acknowledged'] for row in rows), confirmed=sum(row['confirmed'] for row in rows),
                       applied=sum(row['applied'] is True for row in rows),
                       not_applied=sum(row['applied'] is False for row in rows),
                       unverified=sum(row['applied'] is None for row in rows),
                       unconfirmed_applied=sum(row['applied'] is True and not row['confirmed'] for row in rows),
                       not_submitted=sum(not row['posted'] for row in rows),
                       mutations_verified=sum(row['mutation_ok'] for row in rows))
        self.report.update(summary=summary, exit_code=exit_code, interrupted=self.stop.is_set(), status='PASS' if exit_code == 0 else 'FAIL',
                           finished_at=datetime.now().astimezone().isoformat())
        self.emit('Final verification report')
        for row in rows:
            self.emit(f'uid={row["uid"]} fid={row["fid"]}: posted={row["posted"]} acknowledged={row["acknowledged"]} '
                      f'confirmed={row["confirmed"]} applied={row["applied"]} event={row["event_status"]} '
                      f'mutations={"PASS" if row["mutation_ok"] else "FAIL"}')
        self.emit(' '.join(f'{key}={value}' for key, value in summary.items()))
        self.emit(f'{self.report["status"]}: exit={exit_code}; report={self.workdir / "report.json"}')
        destination = self.workdir / 'report.json'
        temporary = destination.with_suffix('.json.tmp')
        temporary.write_text(json.dumps(self.report, indent=2)+'\n')
        temporary.replace(destination)
        (self.workdir / 'exit_status').write_text(str(exit_code)+'\n')
        self.transcript.close()

    def execute(self):
        exit_code = 2
        ready = False
        try:
            self.emit(f'Evidence directory: {self.workdir}')
            self.preflight()
            ready = True
            self.report['phase'] = 'submission'
            if not self.report['fences']:
                self.emit('No fences selected. No leave requests sent.')
            with ThreadPoolExecutor(max_workers=max(1, len(self.clients))) as workers:
                futures = [workers.submit(self.leave, client) for client in self.clients]
                for future in futures:
                    try:
                        future.result()
                    except Exception as exc:
                        self.error(f'client worker: {exc}')
            exit_code = 1
        except Exception as exc:
            self.error(f'{self.report["phase"]}: {exc}')
        finally:
            if ready:
                try:
                    self.verify()
                except Exception as exc:
                    self.error(f'verification: {exc}')
            self.capture_logs()
            if ready:
                successful = (not self.report['errors'] and not self.stop.is_set()
                              and all(batch['status'] == 'PASS' for batch in self.report['batches'])
                              and all(row['posted'] and row['acknowledged'] and row['confirmed'] and row['mutation_ok']
                                      for row in self.report['fences']))
                exit_code = 0 if successful else 1
            if self.stop.is_set():
                exit_code = 130
            self.finalize(exit_code)
        return exit_code


def main(argv=None):
    args = arguments(argv)
    if args.list_fences:
        proof = LeaveProof(args)
        try:
            home = proof.profile(args.account)
            with tempfile.TemporaryDirectory(prefix='list_fences_') as directory:
                log = Path(directory) / 'identity.log'
                rc, status = proof.cli_run(home, log)
                if rc or status != 'EXITED':
                    raise ValueError(f'identity handshake {status}, exit={rc}')
                uid = identity(log.read_text(errors='replace'))
            for fid in user_fences(proof.redis('session', [['ZRANGE', f'UF:{uid}', 0, -1]])[0]):
                print(fid)
            return 0
        except Exception as exc:
            print(f'FAIL: {exc}', file=sys.stderr)
            return 2
    args.evidence_dir.mkdir(parents=True, exist_ok=True)
    workdir = Path(tempfile.mkdtemp(prefix='leave_', dir=args.evidence_dir))
    proof = LeaveProof(args, workdir)
    previous = {}
    def interrupted(signum, frame):
        proof.stop.set()
    for signum in (signal.SIGINT, signal.SIGTERM):
        previous[signum] = signal.signal(signum, interrupted)
    try:
        return proof.execute()
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)


if __name__ == '__main__':
    sys.exit(main())
