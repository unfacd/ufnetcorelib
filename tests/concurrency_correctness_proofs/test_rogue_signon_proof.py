import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import socket
import tempfile
import threading
import unittest
from unittest.mock import patch

import rogue_signon_proof as proof
from test_state_sync_proof import MESSAGE, RECORD, envelope, frame


class RogueSignonTests(unittest.TestCase):
    def fixture(self, response, workers=1, variant='valid'):
        with socket.socket() as server:
            server.bind(('127.0.0.1', 0))
            server.listen(workers)
            server.settimeout(3)
            context = {'target': server.getsockname(), 'cookie': 'private-cookie', 'uid': 663, 'forwarded_for': '127.0.0.1'}
            requests = []
            errors = []
            children = []

            def handle(connection, index):
                try:
                    with connection:
                        connection.settimeout(3)
                        request = b''
                        while not request.endswith(b'\r\n\r\n'):
                            request += connection.recv(4096)
                        requests.append(request)
                        key = re.search(rb'Sec-WebSocket-Key: ([^\r]+)', request)[1]
                        accept = base64.b64encode(hashlib.sha1(key + b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
                        header = b'HTTP/1.1 101 Switching Protocols\r\nSec-WebSocket-Accept: ' + accept + b'\r\n\r\n'
                        if response == 'reject':
                            connection.sendall(header + frame(envelope({'session_state': 1})))
                        elif response == 'bad_accept':
                            connection.sendall(header.replace(accept, b'wrong') + frame(envelope(MESSAGE)))
                        elif response == 'rate_limit':
                            connection.sendall(b'HTTP/1.1 429 Too Many Requests\r\n\r\n')
                        elif response == 'all_accept' or response == 'election' and index == 0:
                            connection.sendall(header + frame(envelope(MESSAGE)))
                except Exception as error:
                    errors.append(error)

            def serve():
                try:
                    for index in range(workers):
                        child = threading.Thread(target=handle, args=(server.accept()[0], index))
                        child.start()
                        children.append(child)
                except Exception as error:
                    errors.append(error)

            thread = threading.Thread(target=serve)
            thread.start()
            results = proof.wave(context, workers, 2, variant)
            for _, peer in results:
                if peer:
                    peer.close()
            thread.join(3)
            for child in children:
                child.join(3)
            self.assertFalse(thread.is_alive())
            self.assertEqual(errors, [])
            self.assertEqual(len(requests), workers)
            return results, requests

    def test_barrier_election_keeps_all_workers_for_one_account(self):
        results, requests = self.fixture('election', workers=10)
        self.assertEqual(sum(peer is not None for _, peer in results), 1)
        self.assertEqual(sum(result['outcome'] == 'closed' for result, _ in results), 9)
        self.assertEqual(len({result['request_key'] for result, _ in results}), 10)
        self.assertTrue(all(b'Cookie: private-cookie\r\n' in request for request in requests))

    def test_invalid_cookie_statesync_is_rejection_not_authentication(self):
        results, requests = self.fixture('reject', variant='invalid-cookie')
        self.assertEqual(results[0][0]['outcome'], 'state_rejected')
        self.assertIsNone(results[0][1])
        self.assertNotIn(b'private-cookie', requests[0])

    def test_missing_cookie_is_actually_omitted(self):
        results, requests = self.fixture('close', variant='missing-cookie')
        self.assertEqual(results[0][0]['outcome'], 'closed')
        self.assertNotIn(b'Cookie:', requests[0])

    def test_bad_accept_and_rate_limit_cannot_pass_as_auth_rejection(self):
        for mode, outcome in (('bad_accept', 'error'), ('rate_limit', 'http_other')):
            with self.subTest(mode=mode):
                results, _ = self.fixture(mode)
                self.assertEqual(results[0][0]['outcome'], outcome)
                self.assertIsNone(results[0][1])

    def test_multiple_authenticated_connections_remain_visible(self):
        results, _ = self.fixture('all_accept', workers=3)
        self.assertEqual(sum(peer is not None for _, peer in results), 3)

    def test_accepted_identity_must_match_redis(self):
        args = argparse.Namespace()
        context = {'uid': 663}
        peer = unittest.mock.Mock(message=MESSAGE)
        peer.live.return_value = True
        with patch.object(proof, 'snapshot', return_value=RECORD):
            proof.check_peer(args, context, peer)
        with patch.object(proof, 'snapshot', return_value=dict(RECORD, uname='other')):
            with self.assertRaisesRegex(ValueError, 'Redis UID record'):
                proof.check_peer(args, context, peer)
        peer.live.return_value = False
        with self.assertRaisesRegex(ValueError, 'disconnected'):
            proof.check_peer(args, context, peer)

    def test_process_fingerprint_ignores_cpu_changes_but_keeps_start_time(self):
        args = argparse.Namespace(procname='server')
        suffix = ['S'] + ['0'] * 18 + ['1234'] + ['0'] * 10
        data = ('42 (server worker) ' + ' '.join(suffix) + '\n').encode()
        with patch.object(proof, 'server_command', return_value=data):
            self.assertEqual(proof.fingerprint(args), [(42, '1234')])
        with patch.object(proof, 'server_command', return_value=b''):
            with self.assertRaisesRegex(ValueError, 'not running'):
                proof.fingerprint(args)

    def test_execute_rejects_double_accept_and_cleans_up_all_peers(self):
        args = argparse.Namespace(account=1, workers=2, timeout=1)
        peers = [unittest.mock.Mock(), unittest.mock.Mock()]
        with tempfile.TemporaryDirectory() as directory, patch.object(proof, 'profile', return_value={'uid': 663, 'target': ('host', 8080)}), patch.object(proof, 'fingerprint', return_value=[(1, '123')]), patch.object(proof, 'log_size', return_value=0), patch.object(proof, 'log_window', return_value=''), patch.object(proof, 'wave', return_value=[({'outcome': 'accepted'}, peer) for peer in peers]):
            with self.assertRaisesRegex(ValueError, 'observed 2'):
                proof.execute(args, Path(directory), {})
            for peer in peers:
                peer.close.assert_called_once()


if __name__ == '__main__':
    unittest.main()
