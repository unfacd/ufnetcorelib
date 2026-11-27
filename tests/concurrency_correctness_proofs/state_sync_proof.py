import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import math
import os
from pathlib import Path
import re
import selectors
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

from bulk_fence_leave_proof import redis_csv


ALPHABET = '0123456789ABCDEFGHJKMNPQRSTVWXYZ'
FIELDS = ('uid_hex', 'cid', 'events_counter', 'uname', 'nickname', 'status')
LIMIT = 4 * 1024 * 1024


def uid_bytes(value):
    if not isinstance(value, str) or len(value) != 26 or any(c not in ALPHABET for c in value):
        raise ValueError('invalid encoded UfsrvUid')
    number = 0
    for character in value:
        number = number * 32 + ALPHABET.index(character)
    if number >= 1 << 128:
        raise ValueError('UfsrvUid overflow')
    return number.to_bytes(16, 'big')


def lua_value(text, key):
    pattern = rf'\b{re.escape(key)}\s*=\s*("(?:\\.|[^"\\])*"|[0-9]+)\s*[,}}]'
    matches = re.findall(pattern, text)
    if len(matches) != 1:
        raise ValueError(f'expected one literal Lua value for {key}')
    token = matches[0]
    return json.loads(token) if token.startswith('"') else int(token)


def varint(data, offset):
    value = 0
    for shift in range(0, 70, 7):
        if offset >= len(data):
            raise ValueError('truncated protobuf varint')
        byte = data[offset]
        offset += 1
        if shift == 63 and byte > 1:
            raise ValueError('protobuf varint overflow')
        value |= (byte & 127) << shift
        if not byte & 128:
            return value, offset
    raise ValueError('protobuf varint overflow')


def protobuf(data):
    fields = {}
    offset = 0
    while offset < len(data):
        tag, offset = varint(data, offset)
        field, wire = tag >> 3, tag & 7
        if not field:
            raise ValueError('protobuf field zero')
        if wire == 0:
            value, offset = varint(data, offset)
        elif wire in (1, 2, 5):
            if wire == 2:
                size, offset = varint(data, offset)
            else:
                size = 8 if wire == 1 else 4
            if size > len(data) - offset:
                raise ValueError('truncated protobuf field')
            value = data[offset:offset + size]
            offset += size
        else:
            raise ValueError('unsupported protobuf wire type')
        if field in fields and field != 5:
            raise ValueError('duplicate protobuf field')
        fields[field] = value
    return fields


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate JSON field: {key}')
        result[key] = value
    return result


def state_sync(payload):
    envelope = protobuf(payload)
    command = envelope.get(2, b'').decode('utf-8')
    if command not in ('/v1/StateSync', 'ufsrv://v1/StateSync'):
        return None
    if envelope.get(1) != 1 or not isinstance(envelope.get(3), bytes):
        raise ValueError('initial StateSync must be an unsolicited request')
    request = protobuf(envelope[3])
    body = request.get(2) or request.get(3)
    if not isinstance(body, bytes) or not body:
        raise ValueError('StateSync request has no JSON body')
    value = json.loads(body, object_pairs_hook=unique_object,
                       parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
    if not isinstance(value, dict):
        raise ValueError('StateSync JSON must be an object')
    return value


class ServerStream:
    def __init__(self, client=False):
        self.client = client
        self.closed = False
        self.buffer = bytearray()
        self.handshake = False
        self.fragment = None

    def feed(self, data):
        self.buffer.extend(data)
        if len(self.buffer) > LIMIT:
            raise ValueError('server stream exceeds capture limit')
        if not self.handshake:
            end = self.buffer.find(b'\r\n\r\n')
            if end < 0:
                return []
            headers = bytes(self.buffer[:end])
            valid = headers.startswith(b'GET ') if self.client else bool(re.match(rb'HTTP/1\.[01] 101(?: |\r|$)', headers))
            if not valid:
                raise ValueError('websocket handshake was rejected')
            del self.buffer[:end + 4]
            self.handshake = True
        messages = []
        while len(self.buffer) >= 2:
            first, second = self.buffer[:2]
            final, opcode = bool(first & 128), first & 15
            if first & 112 or bool(second & 128) != self.client:
                raise ValueError('unsupported RSV bits or incorrect websocket masking')
            size = second & 127
            offset = 2
            if size in (126, 127):
                count = 2 if size == 126 else 8
                if len(self.buffer) < offset + count:
                    break
                size = int.from_bytes(self.buffer[offset:offset + count], 'big')
                offset += count
            if size > LIMIT:
                raise ValueError('websocket frame exceeds capture limit')
            mask = None
            if self.client:
                if len(self.buffer) < offset + 4:
                    break
                mask = bytes(self.buffer[offset:offset + 4])
                offset += 4
            if len(self.buffer) < offset + size:
                break
            payload = bytes(self.buffer[offset:offset + size])
            if mask is not None:
                payload = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
            del self.buffer[:offset + size]
            if opcode in (8, 9, 10):
                if not final or size > 125:
                    raise ValueError('invalid control frame')
                if opcode == 8:
                    self.closed = True
                continue
            if opcode in (1, 2):
                if self.fragment is not None:
                    raise ValueError('overlapping websocket messages')
                self.fragment = bytearray(payload)
            elif opcode == 0 and self.fragment is not None:
                self.fragment.extend(payload)
            else:
                raise ValueError('invalid websocket continuation/opcode')
            if len(self.fragment) > LIMIT:
                raise ValueError('websocket message exceeds capture limit')
            if final:
                messages.append(bytes(self.fragment))
                self.fragment = None
        return messages


class KeepaliveLedger:
    def __init__(self):
        self.sent = {}
        self.acknowledged = {}

    def observe(self, payload, client, now):
        envelope = protobuf(payload)
        if envelope.get(2) != b'/v1/keepalive':
            return
        if client:
            if envelope.get(1) != 1 or not isinstance(envelope.get(3), bytes):
                raise ValueError('keepalive must be a request')
            request = protobuf(envelope[3])
            request_id = request.get(4)
            if type(request_id) is not int or request_id <= 0 or request_id in self.sent:
                raise ValueError('invalid or duplicate keepalive request ID')
            if self.sent and request_id <= max(self.sent):
                raise ValueError('keepalive request IDs must increase')
            self.sent[request_id] = now
        else:
            if envelope.get(1) != 2 or not isinstance(envelope.get(4), bytes):
                raise ValueError('keepalive acknowledgement must be a response')
            response = protobuf(envelope[4])
            request_id = response.get(1)
            if request_id not in self.sent or request_id in self.acknowledged or response.get(2) != 200:
                raise ValueError('unmatched, duplicate or unsuccessful keepalive acknowledgement')
            self.acknowledged[request_id] = now

    def report(self):
        return [{'id': request_id, 'sent_at': sent, 'ack_at': self.acknowledged.get(request_id),
                 'latency_seconds': self.acknowledged[request_id] - sent if request_id in self.acknowledged else None}
                for request_id, sent in self.sent.items()]


def snapshot(args, uid):
    if args.local:
        command = [os.environ.get('REDIS_CLI', 'redis-cli'), '-h', os.environ.get('REDIS_HOST', '127.0.0.1'),
                   '-p', os.environ.get('SESSION_PORT', '63791')]
    else:
        command = ['docker', 'exec', args.redis_session, 'redis-cli']
    script = "local u=redis.call('HGET',KEYS[1],'uid'); if not u then return redis.error_reply('UID record missing') end; local h=(u:gsub('.',function(c) return string.format('%02x',string.byte(c)) end)); local v=redis.call('HMGET',KEYS[1],'cid','events_counter','uname','nickname','status'); table.insert(v,1,h); return v"
    result = subprocess.run(command + ['--csv', 'EVAL', script, '1', f'UID:{uid}'],
                            capture_output=True, text=True, timeout=min(args.timeout, 10))
    if result.returncode:
        raise ValueError(f'Redis command failed: {result.stderr.strip()}')
    values = redis_csv(result.stdout.strip())
    if len(values) != len(FIELDS) or any(value is None for value in values):
        raise ValueError('Redis UID record is incomplete')
    return dict(zip(FIELDS, values))


def db_snapshot(args, uid):
    if type(uid) is not int or not 0 < uid < 1 << 64:
        raise ValueError('invalid numeric UID for account query')
    query = ("SELECT JSON_OBJECT('id',id,'uid_hex',LOWER(HEX(ufsrvuid)),"
             "'username',JSON_UNQUOTE(JSON_EXTRACT(data,'$.number')),"
             "'nickname',JSON_UNQUOTE(JSON_EXTRACT(data_user,'$.nickname'))) "
             f"FROM accounts WHERE id={uid};")
    env = os.environ.copy()
    if args.local:
        command = [os.environ.get('DB_CLI', 'mariadb')]
        defaults = os.environ.get('DB_DEFAULTS_FILE')
        if defaults:
            command.append(f'--defaults-extra-file={defaults}')
        else:
            command += ['--host', os.environ.get('DB_HOST', '127.0.0.1'),
                        '--port', os.environ.get('DB_PORT', '33061'),
                        '--user', os.environ.get('DB_USER', 'ufsrv')]
        command += ['--database', os.environ.get('DB_NAME', 'ufsrv'), '--batch', '--raw',
                    '--skip-column-names', '--connect-timeout=5']
        if 'DB_PASSWORD' in env:
            env['MYSQL_PWD'] = env['DB_PASSWORD']
    else:
        command = ['docker', 'exec', '-i']
        if 'DB_NAME' in env:
            command += ['-e', 'PROOF_DB_NAME=' + env['DB_NAME']]
        command += [args.db_container, 'sh', '-c',
                    'db_user="${MARIADB_USER:-${MYSQL_USER:-root}}"; '
                    'if [ "$db_user" = root ]; then '
                    'MYSQL_PWD="${MARIADB_ROOT_PASSWORD:-${MYSQL_ROOT_PASSWORD:-}}"; '
                    'else MYSQL_PWD="${MARIADB_PASSWORD:-${MYSQL_PASSWORD:-}}"; fi; '
                    'export MYSQL_PWD; exec mariadb --batch --raw --skip-column-names '
                    '--connect-timeout=5 --user="$db_user" '
                    '--database="${PROOF_DB_NAME:-${MARIADB_DATABASE:-${MYSQL_DATABASE:-ufsrv}}}"']
    result = subprocess.run(command, input=query + '\n', capture_output=True, text=True,
                            timeout=min(args.timeout, 10), env=env)
    if result.returncode:
        raise ValueError(f'MariaDB account lookup failed (exit={result.returncode}); check connection and credentials')
    lines = result.stdout.splitlines()
    if len(lines) != 1:
        raise ValueError(f'MariaDB expected one account row for UID {uid}; received {len(lines)}')
    record = json.loads(lines[0], object_pairs_hook=unique_object)
    if not isinstance(record, dict) or set(record) != {'id', 'uid_hex', 'username', 'nickname'}:
        raise ValueError('MariaDB account lookup returned an invalid record')
    if type(record['id']) is not int or not isinstance(record['uid_hex'], str) or not re.fullmatch(r'[0-9a-f]{32}', record['uid_hex']):
        raise ValueError('MariaDB account has an invalid UID')
    if any(not isinstance(record[key], str) for key in ('username', 'nickname')):
        raise ValueError('MariaDB account is missing username or nickname')
    return record


def compare_db(message, redis_record, db_record, uid):
    rows = [{'field': 'db.id', 'passed': type(db_record.get('id')) is int and db_record['id'] == uid}]
    for key, redis_key in (('ufsrvuid', 'uid_hex'), ('username', 'uname'), ('nickname', 'nickname')):
        actual = message.get(key)
        expected = db_record.get('uid_hex' if key == 'ufsrvuid' else key)
        redis_value = redis_record.get(redis_key)
        if key == 'ufsrvuid':
            try:
                actual = uid_bytes(actual).hex()
            except ValueError:
                actual = None
        ok = isinstance(actual, str) and isinstance(expected, str) and actual == expected == redis_value
        rows.append({'field': f'three_way.{key}', 'passed': ok})
    return rows


def compare(message, record, uid):
    expected = {'ufsrvuid': bytes.fromhex(record['uid_hex']), 'cid': int(record['cid']),
                'eid': int(record['events_counter']), 'username': record['uname'], 'nickname': record['nickname'],
                'account_state': True, 'sid': '000'}
    rows = []
    for key, wanted in expected.items():
        actual = message.get(key)
        if key == 'ufsrvuid':
            try:
                actual = uid_bytes(actual)
            except ValueError:
                actual = None
        ok = type(actual) is type(wanted) and actual == wanted
        rows.append({'field': key, 'passed': ok})
    rows.append({'field': 'redis.status', 'passed': str(record['status']) == '1'})
    rows.append({'field': 'redis.uid.sequence', 'passed': len(expected['ufsrvuid']) == 16 and
                 int.from_bytes(expected['ufsrvuid'][8:], 'little') == uid})
    rows.append({'field': 'cid.positive', 'passed': type(message.get('cid')) is int and message['cid'] > 0})
    return rows


def persisted(text, message):
    rows = []
    for key in ('ufsrvuid', 'username', 'cid', 'eid'):
        try:
            value = lua_value(text, key)
            ok = type(value) is type(message.get(key)) and value == message[key]
        except ValueError:
            ok = False
        rows.append({'field': f'cli.prefs.{key}', 'passed': ok})
    return rows


def run(args, evidence, report):
    source = args.home_base / str(args.account)
    profile = evidence / 'client'
    profile.mkdir(mode=0o700)
    for name in ('ufsrvcli.config', 'ufsrvcli.prefs'):
        shutil.copyfile(source / name, profile / name)
        (profile / name).chmod(0o600)
    prefs_path = profile / 'ufsrvcli.prefs'
    original = prefs_path.read_text()
    identity = uid_bytes(lua_value(original, 'ufsrvuid'))
    uid = int.from_bytes(identity[8:], 'little')
    if not uid or args.uid is not None and args.uid != uid:
        raise ValueError('--uid does not match the selected account profile')
    report['uid'] = uid
    report['redis_key'] = f'UID:{uid}'
    config_path = profile / 'ufsrvcli.config'
    config = config_path.read_text()
    match = re.search(r'ufsrvwebsock_backend\s*=\s*\{([^{}]*)\}', config)
    if not match:
        raise ValueError('expected a literal ufsrvwebsock_backend config table')
    backend = match[1]
    if re.search(r'\btls\s*=\s*true\b', backend):
        raise ValueError('wire capture currently requires a plaintext websocket profile')
    target = (lua_value(backend + '}', 'address'), lua_value(backend + '}', 'port'))
    report['target'] = list(target)
    snapshot(args, uid)
    with socket.socket() as listener, selectors.DefaultSelector() as selector:
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        listener.setblocking(False)
        port = listener.getsockname()[1]
        backend = re.sub(r'\baddress\s*=\s*"[^"\n]*"', 'address = "127.0.0.1"', backend)
        backend = re.sub(r'\bport\s*=\s*[0-9]+', f'port = {port}', backend)
        config_path.write_text(config[:match.start(1)] + backend + config[match.end(1):])
        poisoned = re.sub(r'\b(cid|eid)\s*=\s*[0-9]+', r'\1 = 9223372036854775807', original)
        prefs_path.write_text(poisoned)
        selector.register(listener, selectors.EVENT_READ, 'accept')
        stream = ServerStream()
        client_stream = ServerStream(client=True)
        ledger = KeepaliveLedger()
        required_keepalives = getattr(args, 'keepalives', 0)
        sockets = []
        message = None
        verified_at = None
        deadline = time.monotonic() + args.timeout
        with (evidence / 'cli.log').open('wb') as log:
            command = [args.cli, '--headless', '--home-base', str(profile), '--persistent']
            if required_keepalives:
                command += ['--keepalive-interval-ms', str(args.interval_ms),
                            '--keepalive-iterations', str(required_keepalives)]
                report['keepalive_interval_ms'] = args.interval_ms
                report['keepalive_iterations'] = required_keepalives
            proc = subprocess.Popen(command + ['--'],
                                    stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                while time.monotonic() < deadline:
                    code = proc.poll()
                    if code is not None:
                        report['cli_exit'] = code
                        raise ValueError(f'CLI exited before proof completion (exit={code})')
                    for key, _ in selector.select(0.05):
                        if key.data == 'accept':
                            client, _ = listener.accept()
                            try:
                                upstream = socket.create_connection(target, timeout=min(args.timeout, 5))
                            except BaseException:
                                client.close()
                                raise
                            sockets.extend((client, upstream))
                            client.settimeout(2)
                            upstream.settimeout(2)
                            selector.unregister(listener)
                            selector.register(client, selectors.EVENT_READ, upstream)
                            selector.register(upstream, selectors.EVENT_READ, client)
                            continue
                        data = key.fileobj.recv(65536)
                        if not data:
                            raise ValueError('websocket connection closed before proof completion')
                        if key.fileobj is sockets[1]:
                            for payload in stream.feed(data):
                                if required_keepalives:
                                    ledger.observe(payload, False, time.monotonic())
                                value = state_sync(payload)
                                if value is not None:
                                    if message is not None:
                                        raise ValueError('multiple initial StateSync messages')
                                    message = value
                                    record = snapshot(args, uid)
                                    (evidence / 'state_sync.json').write_text(json.dumps(message, indent=2) + '\n')
                                    (evidence / 'redis.json').write_text(json.dumps(record, indent=2) + '\n')
                                    report['checks'] = compare(message, record, uid)
                                    if getattr(args, 'db', False):
                                        db_record = db_snapshot(args, uid)
                                        (evidence / 'mariadb.json').write_text(json.dumps(db_record, indent=2) + '\n')
                                        report['checks'].extend(compare_db(message, record, db_record, uid))
                                    report['not_uid_backed'] = sorted(set(message) - {'ufsrvuid', 'cid', 'eid', 'username', 'nickname', 'account_state', 'sid'})
                        elif required_keepalives:
                            for payload in client_stream.feed(data):
                                ledger.observe(payload, True, time.monotonic())
                        if required_keepalives:
                            report['keepalives'] = ledger.report()
                            if len(ledger.sent) > required_keepalives:
                                raise ValueError('CLI exceeded the requested keepalive iteration count')
                            if stream.closed or client_stream.closed:
                                raise ValueError('websocket closed before persistent proof completion')
                        key.data.sendall(data)
                    if message is not None:
                        rows = persisted(prefs_path.read_text(), message)
                        report['prefs_checks'] = rows
                        if all(row['passed'] for row in rows):
                            verified_at = verified_at or time.monotonic()
                            ready = len(ledger.acknowledged) >= required_keepalives and len(ledger.sent) == len(ledger.acknowledged)
                            if required_keepalives and ready:
                                routed = {int(value) for value in re.findall(r'keepalive acknowledged: id=([0-9]+) status=200', (evidence / 'cli.log').read_text(errors='replace'))}
                                ready = set(ledger.acknowledged).issubset(routed)
                                report['keepalives_routed'] = sorted(routed)
                            if time.monotonic() - verified_at >= 1 and ready:
                                again = snapshot(args, uid)
                                if again != record:
                                    raise ValueError('UID record changed during verification; rerun without concurrent operations')
                                if getattr(args, 'db', False) and db_snapshot(args, uid) != db_record:
                                    raise ValueError('MariaDB account changed during verification; rerun without concurrent operations')
                                if not all(row['passed'] for row in report['checks']):
                                    raise ValueError('StateSync, Redis or MariaDB mismatch; see report checks')
                                report['passed'] = True
                                return
                raise ValueError('deadline expired awaiting StateSync, preference persistence or routed keepalive acknowledgements')
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait(timeout=3)
                report.setdefault('cli_exit', proc.returncode)
                for connection in sockets:
                    connection.close()


def accounts_value(value):
    cells = value.split(',')
    if any(not re.fullmatch(r'[1-9][0-9]*', cell.strip()) for cell in cells):
        raise argparse.ArgumentTypeError('accounts must be comma-separated positive integers')
    accounts = [int(cell.strip()) for cell in cells]
    if len(accounts) != len(set(accounts)):
        raise argparse.ArgumentTypeError('duplicate accounts are not allowed')
    return accounts


def check_profiles(args):
    identities = set()
    cookies = set()
    for account in args.account:
        text = (args.home_base / str(account) / 'ufsrvcli.prefs').read_text()
        identity = uid_bytes(lua_value(text, 'ufsrvuid'))
        cookie = lua_value(text, 'cookie')
        if not isinstance(cookie, str) or not cookie:
            raise ValueError(f'account {account}: missing cookie')
        if identity in identities or cookie in cookies:
            raise ValueError(f'account {account}: selected accounts must have distinct UIDs and cookies')
        identities.add(identity)
        cookies.add(cookie)


def account_proof(args, account, evidence):
    selected = argparse.Namespace(**vars(args))
    selected.account = account
    report = {'account': account, 'passed': False, 'checks': [], 'prefs_checks': [], 'db_enabled': args.db, 'started': time.time()}
    try:
        run(selected, evidence, report)
    except (Exception, KeyboardInterrupt) as error:
        report['error'] = str(error) or 'interrupted'
    finally:
        report['finished'] = time.time()
        (evidence / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description='Prove initial StateSync JSON against UID:<uid> and CLI persistence.')
    parser.add_argument('--home-base', required=True, type=Path)
    parser.add_argument('--account', type=accounts_value, default=[1], help='comma-separated account directories; default 1')
    parser.add_argument('--workers', type=int, nargs='?', const=None, default=1,
                        help='concurrent distinct accounts; no value means one per account; default 1 (serial)')
    parser.add_argument('--uid', type=int, help='optional expected numeric UID for a single account')
    parser.add_argument('--local', action='store_true')
    parser.add_argument('--db', action='store_true', help='also compare account UID, username and nickname with MariaDB')
    parser.add_argument('--db-container', default=os.environ.get('DB_CONTAINER', 'ufsrv-db'))
    parser.add_argument('--redis-session', default=os.environ.get('REDIS_SESSION', 'redis-session'))
    parser.add_argument('--cli', default=os.environ.get('CLI', str(Path(__file__).resolve().parents[3] / 'ufsrvcli/build/dev/src/ufsrvcli')))
    parser.add_argument('--timeout', type=float, help='deadline; default 20s, or interval times iterations plus 15s')
    parser.add_argument('--iterations', '--keepalives', '--keepalive-iterations', dest='keepalives', type=int, default=0, help='number of keepalives to send and verify')
    parser.add_argument('--interval-ms', '--keepalive-interval-ms', dest='interval_ms', type=int, default=25000, help='CLI keepalive period in milliseconds; default 25000')
    parser.add_argument('--evidence-dir', type=Path, default=Path('/tmp/concurrency_correctness_proofs'))
    args = parser.parse_args(argv)
    if not 0 <= args.keepalives <= 2147483647 or not 1 <= args.interval_ms <= 2147483647:
        parser.error('iterations must be in 0..2147483647 and interval-ms in 1..2147483647')
    if args.timeout is None:
        args.timeout = args.interval_ms / 1000 * args.keepalives + 15 if args.keepalives else 20
    if args.workers is None:
        args.workers = len(args.account)
    if args.workers < 1 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('workers and timeout must be positive and finite')
    if args.uid is not None and len(args.account) != 1:
        parser.error('--uid can only be used with one account; UIDs are otherwise read from prefs')
    os.umask(0o077)
    args.evidence_dir.mkdir(parents=True, exist_ok=True)
    evidence = Path(tempfile.mkdtemp(prefix='state_sync_', dir=args.evidence_dir))
    workers = min(args.workers, len(args.account))
    summary = {'passed': False, 'workers_requested': args.workers, 'workers_launched': workers,
               'started': time.time(), 'accounts': []}
    print(f'Evidence directory: {evidence}', flush=True)
    try:
        check_profiles(args)
        destinations = []
        for account in args.account:
            directory = evidence if len(args.account) == 1 else evidence / f'account_{account}'
            directory.mkdir(mode=0o700, exist_ok=True)
            destinations.append(directory)
        with ThreadPoolExecutor(max_workers=workers) as executor:
            futures = [executor.submit(account_proof, args, account, directory)
                       for account, directory in zip(args.account, destinations)]
            summary['accounts'] = [future.result() for future in futures]
        summary['passed'] = all(report['passed'] for report in summary['accounts'])
    except (Exception, KeyboardInterrupt) as error:
        summary['error'] = str(error) or 'interrupted'
        print(f'FAIL: {summary["error"]}', flush=True)
    finally:
        summary['finished'] = time.time()
        if len(args.account) == 1 and summary['accounts']:
            output = dict(summary['accounts'][0], workers_requested=args.workers, workers_launched=workers)
        else:
            output = summary
        (evidence / 'report.json').write_text(json.dumps(output, indent=2) + '\n')
    for report in summary['accounts']:
        account = report['account']
        for row in report['checks'] + report['prefs_checks']:
            print(f'account={account} {"PASS" if row["passed"] else "FAIL"}: {row["field"]}')
        if 'error' in report:
            print(f'account={account} FAIL: {report["error"]}')
        print(f'account={account}: {"PASS" if report["passed"] else "FAIL"}')
    print(f'workers: requested={args.workers} launched={workers} (accounts={len(args.account)})')
    if workers < args.workers:
        print('Worker count capped at the number of selected accounts.')
    print(f'{"PASS" if summary["passed"] else "FAIL"}: report={evidence / "report.json"}')
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
