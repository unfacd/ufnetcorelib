import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import select
import socket
import subprocess
import sys
import tempfile
import threading
import time

from state_sync_proof import ServerStream, compare, lua_value, snapshot, state_sync, uid_bytes


class Peer:
    def __init__(self, sock, stream, message):
        self.sock = sock
        self.stream = stream
        self.message = message

    def close(self):
        self.sock.close()

    def live(self):
        if select.select([self.sock], [], [], 0)[0]:
            data = self.sock.recv(65536)
            if not data:
                return False
            if data[0] & 15 == 8:
                return False
            self.stream.feed(data)
        return True


def profile(args, account):
    directory = args.home_base / str(account)
    config = (directory / 'ufsrvcli.config').read_text()
    prefs = (directory / 'ufsrvcli.prefs').read_text()
    backend = re.search(r'ufsrvwebsock_backend\s*=\s*\{([^{}]*)\}', config)
    if not backend:
        raise ValueError('literal websocket backend table required')
    if re.search(r'\btls\s*=\s*true\b', backend[1]):
        raise ValueError('raw handshake proof currently requires a plaintext websocket endpoint')
    target = (lua_value(backend[1] + '}', 'address'), lua_value(backend[1] + '}', 'port'))
    cookie = lua_value(prefs, 'cookie')
    uid = int.from_bytes(uid_bytes(lua_value(prefs, 'ufsrvuid'))[8:], 'little')
    if not isinstance(cookie, str) or not cookie or not uid or any(c in cookie for c in '\r\n\x00'):
        raise ValueError('invalid account UID or cookie')
    forwarded = re.search(r'\bforwarded_for\s*=\s*"([^"\r\n]*)"', backend[1])
    return {'target': target, 'cookie': cookie, 'uid': uid,
            'forwarded_for': forwarded[1] if forwarded else '127.0.0.1'}


def handshake(context, timeout, barrier=None, variant='valid'):
    sock = None
    started = time.monotonic()
    key = base64.b64encode(os.urandom(16)).decode()
    try:
        sock = socket.create_connection(context['target'], timeout=timeout)
        sock.settimeout(timeout)
        cookie = context['cookie'] if variant != 'invalid-cookie' else 'invalid_' + os.urandom(24).hex()
        headers = ['GET / HTTP/1.1', 'Host: ' + context['target'][0], 'Upgrade: websocket',
                   'Connection: Upgrade', 'Sec-WebSocket-Key: ' + key, 'Sec-WebSocket-Version: 13',
                   'X-Forwarded-For: ' + context['forwarded_for'], 'Sec-WebSocket-Protocol: base64']
        if variant != 'missing-cookie':
            headers.append('Cookie: ' + cookie)
        if barrier:
            barrier.wait(timeout=timeout)
        sent = time.monotonic()
        sock.sendall(('\r\n'.join(headers) + '\r\n\r\n').encode())
        stream = ServerStream()
        data = bytearray()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            sock.settimeout(max(0.01, deadline - time.monotonic()))
            chunk = sock.recv(65536)
            if not chunk:
                sock.close()
                return {'request_key': key, 'outcome': 'closed', 'sent_at': sent, 'elapsed': time.monotonic() - started}, None
            if not stream.handshake:
                data.extend(chunk)
                if len(data) > 65536:
                    raise ValueError('oversized handshake response')
                end = data.find(b'\r\n\r\n')
                if end < 0:
                    continue
                first = bytes(data[:end]).split(b'\r\n')[0]
                match = re.fullmatch(rb'HTTP/1\.[01] ([0-9]{3})(?: .*|)', first)
                if not match:
                    raise ValueError('invalid handshake response')
                status = int(match[1])
                if status != 101:
                    sock.close()
                    return {'request_key': key, 'outcome': 'http_rejected' if status in (400, 401, 403, 409) else 'http_other',
                            'status': status, 'sent_at': sent, 'elapsed': time.monotonic() - started}, None
                accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
                values = re.findall(rb'(?im)^Sec-WebSocket-Accept:\s*([^\r\n]+)', bytes(data[:end]))
                if values != [accept]:
                    raise ValueError('incorrect websocket accept value')
                chunk = bytes(data)
            for payload in stream.feed(chunk):
                message = state_sync(payload)
                if message is not None:
                    if type(message.get('session_state')) is int and message['session_state'] == 1:
                        sock.close()
                        return {'request_key': key, 'outcome': 'state_rejected', 'sent_at': sent,
                                'elapsed': time.monotonic() - started}, None
                    if message.get('account_state') is not True or type(message.get('cid')) is not int or message['cid'] <= 0:
                        raise ValueError('StateSync does not establish an authenticated session')
                    uid_bytes(message.get('ufsrvuid'))
                    peer = Peer(sock, stream, message)
                    return {'request_key': key, 'outcome': 'accepted', 'sent_at': sent, 'cid': message.get('cid'),
                            'elapsed': time.monotonic() - started}, peer
        raise TimeoutError('no StateSync before deadline')
    except (OSError, ValueError, TimeoutError, threading.BrokenBarrierError) as error:
        if sock is not None:
            sock.close()
        return {'request_key': key, 'outcome': 'error', 'error': type(error).__name__, 'elapsed': time.monotonic() - started}, None


def wave(context, workers, timeout, variant='valid'):
    barrier = threading.Barrier(workers)
    with ThreadPoolExecutor(max_workers=workers) as executor:
        futures = [executor.submit(handshake, context, timeout, barrier, variant) for _ in range(workers)]
        return [future.result() for future in futures]


def server_command(args, command):
    if not args.local:
        command = ['docker', 'exec', args.container] + command
    result = subprocess.run(command, capture_output=True, timeout=10)
    if result.returncode:
        raise ValueError('server process or log inspection failed')
    return result.stdout


def fingerprint(args):
    data = server_command(args, ['sh', '-c', 'for p in $(pgrep -x "$1"); do cat "/proc/$p/stat"; done', 'sh', args.procname])
    records = []
    for line in data.decode().splitlines():
        prefix, suffix = line.rsplit(')', 1)
        records.append((int(prefix.split(' ', 1)[0]), suffix.split()[19]))
    if not records:
        raise ValueError('server process is not running')
    return sorted(records)


def log_size(args):
    data = server_command(args, ['wc', '-l', args.log]).decode()
    return int(data.split()[0])


def log_window(args, start):
    if log_size(args) < start:
        raise ValueError('server log truncated during run')
    return server_command(args, ['tail', '-n', '+' + str(start + 1), args.log]).decode(errors='replace')


def rejection_count(text):
    markers = ('REJECTING INCOMING CONNECTION', 'TERMINATING THIS INCOMING CONNECTION',
               'COOKIE NOT FOUND: REJECTING', 'REJECTING OVER SIZED COOKIE')
    return sum(any(marker in line for marker in markers) for line in text.splitlines())


def check_peer(args, context, peer, expected_cid=None):
    if not peer.live():
        raise ValueError('accepted session disconnected during the attack')
    if expected_cid is not None and peer.message.get('cid') != expected_cid:
        raise ValueError('incumbent connection ID changed')
    record = snapshot(args, context['uid'])
    checks = compare(peer.message, record, context['uid'])
    if not all(row['passed'] for row in checks):
        raise ValueError('accepted StateSync does not match its Redis UID record')
    return record


def execute(args, evidence, report):
    context = profile(args, args.account)
    report['uid'] = context['uid']
    report['target'] = list(context['target'])
    before = fingerprint(args)
    baseline = log_size(args)
    report['process_before'] = before
    peers = []
    try:
        election = wave(context, args.workers, args.timeout)
        peers.extend(peer for _, peer in election if peer)
        report['election'] = [result for result, _ in election]
        accepted = [peer for _, peer in election if peer]
        if len(accepted) != 1:
            raise ValueError(f'cold sign-on wave expected one accepted session; observed {len(accepted)}')
        if any(result['outcome'] not in ('accepted', 'closed', 'http_rejected', 'state_rejected') for result, _ in election):
            raise ValueError('cold sign-on wave had transport errors, timeout or unexpected HTTP status')
        incumbent = accepted[0]
        report['incumbent_cid'] = incumbent.message['cid']
        report['incumbent_redis'] = check_peer(args, context, incumbent)
        (evidence / 'incumbent_state_sync.json').write_text(json.dumps(incumbent.message, indent=2) + '\n')
        live_baseline = log_size(args)
        for iteration in range(args.iter):
            for variant in args.variants:
                outcomes = wave(context, args.workers, args.timeout, variant)
                peers.extend(peer for _, peer in outcomes if peer)
                report['waves'].append({'iteration': iteration + 1, 'variant': variant,
                                        'results': [result for result, _ in outcomes]})
                if any(peer for _, peer in outcomes):
                    raise ValueError('incoming attacker authenticated while the incumbent remained live')
                if any(result['outcome'] not in ('closed', 'http_rejected', 'state_rejected') for result, _ in outcomes):
                    raise ValueError('attack wave had transport errors, timeout or unexpected HTTP status')
                report['waves'][-1]['incumbent_redis'] = check_peer(args, context, incumbent, report['incumbent_cid'])
                if fingerprint(args) != before:
                    raise ValueError('server process restarted or disappeared during attack')
        expected = {result['request_key'] for item in report['waves'] for result in item['results']}
        log_deadline = time.monotonic() + min(args.timeout, 2)
        while True:
            text = log_window(args, live_baseline)
            received = {match[1] for line in text.splitlines() if 'RECEIVED HANDSHAKE:' in line
                        for match in re.finditer(r'Sec-WebSocket-Key: ([A-Za-z0-9+/=]+)', line)}
            if expected.issubset(received) or time.monotonic() >= log_deadline:
                break
            time.sleep(0.1)
        report['live_requests_logged'] = len(received & expected)
        if not expected.issubset(received):
            raise ValueError('not every attack request appears in the server handshake log')
        report['live_rejection_records'] = rejection_count(text)
        report['live_guard_records'] = sum('REJECTING INCOMING CONNECTION' in line and
                                          f"cid_hashed:'{report['incumbent_cid']}'" in line
                                          for line in text.splitlines())
        if report['live_guard_records'] < 1:
            raise ValueError('no explicit server rejection record; socket closures alone do not prove the guard ran')
        if args.control_account is not None:
            control = profile(args, args.control_account)
            if control['uid'] == context['uid'] or control['cookie'] == context['cookie']:
                raise ValueError('control account must have a distinct UID and cookie')
            result, peer = handshake(control, args.timeout)
            report['control'] = result
            if peer is None:
                raise ValueError('independent control account could not authenticate')
            peers.append(peer)
            report['control_redis'] = check_peer(args, control, peer)
        incumbent.close()
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            result, recovered = handshake(context, min(3, max(0.1, deadline - time.monotonic())))
            report['recovery_attempts'].append(result)
            if recovered:
                peers.append(recovered)
                report['recovery_redis'] = check_peer(args, context, recovered)
                report['recovery_cid'] = recovered.message['cid']
                break
            time.sleep(0.1)
        else:
            raise ValueError('account could not reconnect after incumbent socket closed; possible stranded claim/session')
        if fingerprint(args) != before:
            raise ValueError('server process restarted during proof')
        report['passed'] = True
    finally:
        for peer in peers:
            peer.close()
        report['process_after'] = fingerprint(args)
        text = log_window(args, baseline)
        (evidence / 'server.log').write_text(text)
        observed = {match[1] for line in text.splitlines() if 'RECEIVED HANDSHAKE:' in line
                    for match in re.finditer(r'Sec-WebSocket-Key: ([A-Za-z0-9+/=]+)', line)}
        election_keys = {row['request_key'] for row in report.get('election', []) if 'request_key' in row}
        report['election_requests_logged'] = len(election_keys & observed)
        if report.get('passed') and not election_keys.issubset(observed):
            report['passed'] = False
            raise ValueError('not every election request appears in the server handshake log')
        markers = ('LOCK NOT OWNED BY THIS THREAD', 'COULD NOT RECORD LOCK OWNERSHIP',
                   'ITEM NOT DEQUEUED', 'LOCKEDSESNSTATUS_IOERROR')
        report['safety_errors'] = {marker: text.count(marker) for marker in markers}
        if any(report['safety_errors'].values()):
            report['passed'] = False
            raise ValueError('server log contains lock-ownership or recycler safety errors')
        if report['process_after'] != before:
            report['passed'] = False
            raise ValueError('server process identity changed')


def main(argv=None):
    parser = argparse.ArgumentParser(description='Prove same-cookie sign-on isolation and recovery with direct websocket clients.')
    parser.add_argument('--same-cookie', action='store_true')
    parser.add_argument('--home-base', required=True, type=Path)
    parser.add_argument('--account', type=int, default=1)
    parser.add_argument('--workers', type=int, default=10)
    parser.add_argument('--iter', type=int, default=1)
    parser.add_argument('--timeout', type=float, default=8)
    parser.add_argument('--variants', default='valid', help='comma-separated valid,invalid-cookie,missing-cookie; valid always included')
    parser.add_argument('--control-account', type=int)
    parser.add_argument('--local', action='store_true')
    parser.add_argument('--container', default=os.environ.get('CONTAINER', 'ufsrvwebsock'))
    parser.add_argument('--procname', default=os.environ.get('PROCNAME', 'ufsrvwebsock'))
    parser.add_argument('--log', default='/var/log/ufsrvwebsock.log')
    parser.add_argument('--redis-session', default=os.environ.get('REDIS_SESSION', 'redis-session'))
    parser.add_argument('--evidence-dir', type=Path, default=Path('/tmp/concurrency_correctness_proofs'))
    args = parser.parse_args(argv)
    if args.workers < 1 or args.account < 1 or args.iter < 1 or not 0 < args.timeout < 300:
        parser.error('account, workers and iter must be positive; timeout must be between 0 and 300')
    args.variants = list(dict.fromkeys(['valid'] + args.variants.split(',')))
    if set(args.variants) - {'valid', 'invalid-cookie', 'missing-cookie'}:
        parser.error('unsupported variant')
    os.umask(0o077)
    args.evidence_dir.mkdir(parents=True, exist_ok=True)
    evidence = Path(tempfile.mkdtemp(prefix='rogue_signon_', dir=args.evidence_dir))
    report = {'passed': False, 'started': time.time(), 'account': args.account, 'workers': args.workers,
              'waves': [], 'recovery_attempts': [], 'client': 'direct websocket sockets'}
    print(f'Evidence directory: {evidence}', flush=True)
    try:
        execute(args, evidence, report)
    except (Exception, KeyboardInterrupt) as error:
        report['passed'] = False
        report['error'] = str(error) or 'interrupted'
        print(f'FAIL: {report["error"]}')
    finally:
        report['finished'] = time.time()
        (evidence / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{"PASS" if report["passed"] else "FAIL"}: account={args.account} workers={args.workers}; report={evidence / "report.json"}')
    if 'election' in report:
        accepted = sum(row['outcome'] == 'accepted' for row in report['election'])
        print(f'election: accepted={accepted}/{args.workers}; logged={report.get("election_requests_logged", 0)}')
    for item in report['waves']:
        counts = {outcome: sum(row['outcome'] == outcome for row in item['results'])
                  for outcome in sorted({row['outcome'] for row in item['results']})}
        print(f'wave={item["iteration"]} variant={item["variant"]}: {counts}')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
