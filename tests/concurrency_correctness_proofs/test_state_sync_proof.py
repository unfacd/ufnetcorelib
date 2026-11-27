import argparse
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import socket
import subprocess
import struct
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

import state_sync_proof as proof


UID = '5Z6X6G00000009E0G000000000'
MESSAGE = {'ufsrvuid': UID, 'cid': 1647916098332396415, 'eid': 17,
           'username': 'proof@example.com', 'nickname': '', 'account_state': True, 'sid': '000'}
RECORD = {'uid_hex': proof.uid_bytes(UID).hex(), 'cid': str(MESSAGE['cid']),
          'events_counter': '17', 'uname': MESSAGE['username'], 'nickname': '', 'status': '1'}


def vi(number):
    output = bytearray()
    while number > 127:
        output.append((number & 127) | 128)
        number >>= 7
    return bytes(output) + bytes([number])


def field(number, data):
    return vi(number * 8 + 2) + vi(len(data)) + data


def envelope(message):
    return b'\x08\x01' + field(2, b'/v1/StateSync') + field(3, field(2, json.dumps(message).encode()))


def frame(payload, opcode=2, final=True):
    header = bytes([opcode | (128 if final else 0)])
    size = len(payload)
    return header + (bytes([size]) if size < 126 else b'\x7e' + struct.pack('!H', size)) + payload


class StateSyncProofTests(unittest.TestCase):
    def test_uid_codec_known_account_and_overflow(self):
        self.assertEqual(int.from_bytes(proof.uid_bytes(UID)[8:], 'little'), 663)
        self.assertEqual(proof.uid_bytes('0' * 26), b'\0' * 16)
        for bad in ('Z' * 26, 'I' * 26, '0' * 25, '', None):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                proof.uid_bytes(bad)

    def test_bytewise_handshake_and_fragmented_protobuf_with_ping(self):
        encoded = envelope(MESSAGE)
        stream = proof.ServerStream()
        data = (b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n' +
                frame(encoded[:7], final=False) + frame(b'ping', opcode=9) +
                frame(encoded[7:], opcode=0))
        messages = []
        for byte in data:
            messages.extend(stream.feed(bytes([byte])))
        self.assertEqual([proof.state_sync(item) for item in messages], [MESSAGE])

    def test_malformed_wire_and_duplicate_json_fail(self):
        for data in (b'\x12\xff', b'\x00', b'\x12\x04x', b'\x08' + b'\xff' * 10):
            with self.subTest(data=data), self.assertRaises(ValueError):
                proof.state_sync(data)
        bad = b'\x08\x01' + field(2, b'/v1/StateSync') + field(3, field(3, b'{"cid":1,"cid":2}'))
        with self.assertRaises(ValueError):
            proof.state_sync(bad)
        with self.assertRaises(ValueError):
            proof.ServerStream().feed(b'HTTP/1.1 403 Forbidden\r\n\r\n')
        with self.assertRaises(ValueError):
            proof.ServerStream().feed(b'HTTP/1.1 101 OK\r\n\r\n\x80\x00')

    def test_every_mapped_field_mismatch_and_missing_are_detected(self):
        self.assertTrue(all(row['passed'] for row in proof.compare(MESSAGE, RECORD, 663)))
        for key in MESSAGE:
            for value in (None, False, 0, 'incorrect'):
                changed = dict(MESSAGE, **{key: value})
                with self.subTest(key=key, value=value):
                    self.assertFalse(all(row['passed'] for row in proof.compare(changed, RECORD, 663)))
        self.assertFalse(all(row['passed'] for row in proof.compare(MESSAGE, dict(RECORD, status='0'), 663)))
        self.assertFalse(all(row['passed'] for row in proof.compare(MESSAGE, RECORD, 664)))
        self.assertFalse(all(row['passed'] for row in proof.compare(dict(MESSAGE, cid=MESSAGE['cid'] + 1), RECORD, 663)))

    def test_live_relay_and_cli_persistence_gate(self):
        self.integration('success')

    def test_received_payload_cannot_hide_cli_crash(self):
        self.integration('crash')

    def test_received_payload_without_cli_persistence_times_out(self):
        self.integration('no_persist')

    def test_persisted_payload_cannot_hide_wrong_redis_value(self):
        self.integration('wrong_redis')

    def test_account_list_rejects_duplicates_and_invalid_ids(self):
        self.assertEqual(proof.accounts_value('1, 2,3'), [1, 2, 3])
        for value in ('1,1', '0', '-1', '1,', '', '1.2'):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                proof.accounts_value(value)

    def test_serial_accounts_and_failure_does_not_skip_next_account(self):
        self.schedule(workers=1, expected_peak=1, failed_account=2)

    def test_workers_distribute_distinct_profiles_concurrently(self):
        self.schedule(workers=2, expected_peak=2)

    def test_worker_count_capped_at_accounts(self):
        self.schedule(workers=8, expected_peak=3)

    def test_bare_workers_launches_one_per_account(self):
        self.schedule(workers=None, expected_peak=3)

    def schedule(self, workers, expected_peak, failed_account=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            active = set()
            seen = []
            peak = 0
            lock = threading.Lock()
            barrier = threading.Barrier(expected_peak) if expected_peak > 1 else None

            def execute(args, evidence, report):
                nonlocal peak
                with lock:
                    self.assertNotIn(args.account, active)
                    active.add(args.account)
                    seen.append(args.account)
                    peak = max(peak, len(active))
                if barrier and args.account <= expected_peak:
                    barrier.wait(timeout=3)
                time.sleep(0.03)
                with lock:
                    active.remove(args.account)
                if args.account == failed_account:
                    raise ValueError('fixture failure')
                report['passed'] = True

            with patch.object(proof, 'check_profiles'), patch.object(proof, 'run', side_effect=execute), redirect_stdout(io.StringIO()) as output:
                worker_args = ['--workers'] if workers is None else ['--workers', str(workers)]
                status = proof.main(['--home-base', str(root), '--account', '1,2,3',
                                     '--evidence-dir', str(root)] + worker_args)
            self.assertEqual(status, 1 if failed_account else 0)
            self.assertEqual(peak, expected_peak)
            self.assertEqual(sorted(seen), [1, 2, 3])
            if workers == 1:
                self.assertEqual(seen, [1, 2, 3])
            evidence = next(root.glob('state_sync_*'))
            summary = json.loads((evidence / 'report.json').read_text())
            self.assertEqual(summary['workers_launched'], 3 if workers is None else min(workers, 3))
            self.assertEqual(len(summary['accounts']), 3)
            for account in (1, 2, 3):
                report = json.loads((evidence / f'account_{account}' / 'report.json').read_text())
                self.assertEqual(report['account'], account)
                self.assertEqual(report['passed'], account != failed_account)
            if workers is not None and workers > 3:
                self.assertIn('capped', output.getvalue())

    def test_same_identity_or_cookie_is_rejected_before_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for account in (1, 2):
                profile = root / str(account)
                profile.mkdir()
                (profile / 'ufsrvcli.prefs').write_text(f'return {{ufsrvuid = "{UID}", cookie = "cookie{account}",}}')
            args = argparse.Namespace(home_base=root, account=[1, 2])
            with self.assertRaisesRegex(ValueError, 'distinct UIDs and cookies'):
                proof.check_profiles(args)
            encoded = '00000000000000000000000000'
            (root / '2' / 'ufsrvcli.prefs').write_text(f'return {{ufsrvuid = "{encoded}", cookie = "cookie1",}}')
            with self.assertRaisesRegex(ValueError, 'distinct UIDs and cookies'):
                proof.check_profiles(args)

    def test_db_comparison_detects_each_mismatching_vector(self):
        db = {'id': 663, 'uid_hex': RECORD['uid_hex'], 'username': MESSAGE['username'], 'nickname': ''}
        self.assertTrue(all(row['passed'] for row in proof.compare_db(MESSAGE, RECORD, db, 663)))
        for key, changed in (('id', 664), ('uid_hex', '00' * 16), ('username', 'other'), ('nickname', 'other')):
            with self.subTest(key=key):
                self.assertFalse(all(row['passed'] for row in proof.compare_db(MESSAGE, RECORD, dict(db, **{key: changed}), 663)))
        for message_key, redis_key in (('ufsrvuid', 'uid_hex'), ('username', 'uname'), ('nickname', 'nickname')):
            with self.subTest(vector='redis', key=redis_key):
                self.assertFalse(all(row['passed'] for row in proof.compare_db(MESSAGE, dict(RECORD, **{redis_key: 'other'}), db, 663)))
            with self.subTest(vector='json', key=message_key):
                self.assertFalse(all(row['passed'] for row in proof.compare_db(dict(MESSAGE, **{message_key: 'other'}), RECORD, db, 663)))

    def test_db_lookup_rejects_missing_duplicate_invalid_rows_and_failures(self):
        args = argparse.Namespace(local=False, db_container='test-db', timeout=2)
        db = {'id': 663, 'uid_hex': RECORD['uid_hex'], 'username': 'name\t雪', 'nickname': ''}
        with patch.object(proof.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, json.dumps(db) + '\n', '')) as call:
            self.assertEqual(proof.db_snapshot(args, 663), db)
            self.assertIn('WHERE id=663;', call.call_args.kwargs['input'])
            self.assertIn('test-db', call.call_args.args[0])
        invalid = ('', json.dumps(db) + '\n' + json.dumps(db), '{}',
                   json.dumps(dict(db, uid_hex='bad')), json.dumps(dict(db, nickname=None)))
        for output in invalid:
            with self.subTest(output=output), patch.object(proof.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, output, '')):
                with self.assertRaises(ValueError):
                    proof.db_snapshot(args, 663)
        with patch.object(proof.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, '', 'private credentials')):
            with self.assertRaisesRegex(ValueError, 'MariaDB account lookup failed') as error:
                proof.db_snapshot(args, 663)
            self.assertNotIn('private credentials', str(error.exception))
        with self.assertRaises(ValueError):
            proof.db_snapshot(args, '663 OR 1=1')

    def test_local_db_password_is_not_passed_in_arguments(self):
        args = argparse.Namespace(local=True, timeout=2)
        db = {'id': 663, 'uid_hex': RECORD['uid_hex'], 'username': MESSAGE['username'], 'nickname': ''}
        with patch.dict(proof.os.environ, {'DB_PASSWORD': 'private-value', 'DB_DEFAULTS_FILE': '/private/client.cnf'}), patch.object(proof.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, json.dumps(db), '')) as call:
            self.assertEqual(proof.db_snapshot(args, 663), db)
            self.assertNotIn('private-value', ' '.join(call.call_args.args[0]))
            self.assertEqual(call.call_args.kwargs['env']['MYSQL_PWD'], 'private-value')
            self.assertIn('--defaults-extra-file=/private/client.cnf', call.call_args.args[0])

    def test_three_way_proof_success(self):
        self.integration('db_success')

    def test_three_way_proof_rejects_mismatch(self):
        self.integration('db_mismatch')

    def test_three_way_proof_rejects_unavailable_database(self):
        self.integration('db_error')

    def test_three_way_proof_rejects_database_changes(self):
        self.integration('db_changed')

    def integration(self, mode):
        with tempfile.TemporaryDirectory() as directory, socket.socket() as server:
            root = Path(directory)
            account = root / 'accounts' / '1'
            account.mkdir(parents=True)
            evidence = root / 'evidence'
            evidence.mkdir()
            server.bind(('127.0.0.1', 0))
            server.listen(1)
            server.settimeout(5)
            config = f'ufsrv_cli = {{ufsrvwebsock_backend = {{address = "127.0.0.1", port = {server.getsockname()[1]}}}}}'
            original = f'return {{ufsrvuid = "{UID}", username = "proof@example.com", cid = 1, eid = 0,}}'
            (account / 'ufsrvcli.config').write_text(config)
            (account / 'ufsrvcli.prefs').write_text(original)
            executable = root / 'client'
            executable.write_text('''#!/usr/bin/env python3
import json, pathlib, re, socket, sys, time
home = pathlib.Path(sys.argv[sys.argv.index('--home-base') + 1])
config = (home / 'ufsrvcli.config').read_text()
port = int(re.search(r'port = ([0-9]+)', config)[1])
with socket.create_connection(('127.0.0.1', port)) as connection:
    connection.sendall(b'GET / HTTP/1.1\\r\\n\\r\\n')
    received = b''
    while not received.endswith(b'END'):
        chunk = connection.recv(65536)
        if not chunk: sys.exit(5)
        received += chunk
    MODE
    time.sleep(10)
'''.replace('MODE', 'sys.exit(23)' if mode == 'crash' else 'pass' if mode == 'no_persist' else
            '(home / "ufsrvcli.prefs").write_text(' + repr('return {' + ', '.join(f'{key} = {json.dumps(MESSAGE[key])}' for key in ('ufsrvuid', 'username', 'cid', 'eid')) + ',}') + ')'))
            executable.chmod(0o700)
            failures = []

            def serve():
                try:
                    with server.accept()[0] as connection:
                        connection.settimeout(5)
                        connection.recv(4096)
                        connection.sendall(b'HTTP/1.1 101 Switching Protocols\r\n\r\n' + frame(envelope(MESSAGE)) + frame(b'END', opcode=9))
                        while connection.recv(4096):
                            pass
                except Exception as error:
                    failures.append(error)

            thread = threading.Thread(target=serve)
            thread.start()
            args = argparse.Namespace(home_base=root / 'accounts', account=1, uid=None, timeout=1.7,
                                      cli=str(executable), local=True, db=mode.startswith('db_'))
            report = {'passed': False}
            record = dict(RECORD, events_counter='18') if mode == 'wrong_redis' else RECORD
            db = {'id': 663, 'uid_hex': RECORD['uid_hex'], 'username': MESSAGE['username'], 'nickname': ''}
            db_changed = dict(db, nickname='changed')
            db_values = ([db_changed, db_changed] if mode == 'db_mismatch' else [ValueError('database unavailable')]
                         if mode == 'db_error' else [db, db_changed] if mode == 'db_changed' else [db, db])
            with patch.object(proof, 'snapshot', return_value=record), patch.object(proof, 'db_snapshot', side_effect=db_values):
                if mode in ('success', 'db_success'):
                    proof.run(args, evidence, report)
                    self.assertTrue(report['passed'])
                    self.assertTrue(all(row['passed'] for row in report['prefs_checks']))
                else:
                    with self.assertRaises(ValueError):
                        proof.run(args, evidence, report)
                    self.assertFalse(report['passed'])
            thread.join(5)
            self.assertFalse(thread.is_alive())
            self.assertEqual(failures, [])
            self.assertEqual((account / 'ufsrvcli.config').read_text(), config)
            self.assertEqual((account / 'ufsrvcli.prefs').read_text(), original)
            self.assertTrue((evidence / 'state_sync.json').is_file())


if __name__ == '__main__':
    unittest.main()
