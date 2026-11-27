import unittest
from contextlib import redirect_stdout, redirect_stderr
import io
import tempfile
from unittest.mock import patch
import state_sync_proof as proof

from state_sync_proof import KeepaliveLedger, ServerStream
from test_state_sync_proof import field, frame, vi


def keepalive(request_id, response=False, status=200):
    content = vi(8 if response else 32) + vi(request_id)
    if response:
        content += vi(16) + vi(status)
    return vi(8) + vi(2 if response else 1) + field(2, b'/v1/keepalive') + field(4 if response else 3, content)


def masked_frame(payload):
    mask = b'\x01\x80\x00\xff'
    wire = bytearray(frame(bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))))
    wire[1] |= 128
    return bytes(wire[:2]) + mask + bytes(wire[2:])


class PersistentWebsocketTests(unittest.TestCase):
    def test_interval_and_iteration_switches_set_cli_controls_and_deadline(self):
        with tempfile.TemporaryDirectory() as root, patch.object(proof, 'check_profiles'), patch.object(proof, 'account_proof', return_value={'account': 1, 'passed': True, 'checks': [], 'prefs_checks': []}) as worker, redirect_stdout(io.StringIO()):
            self.assertEqual(proof.main(['--home-base', root, '--evidence-dir', root,
                                         '--interval-ms', '17', '--iterations', '3']), 0)
            args = worker.call_args.args[0]
            self.assertEqual(args.interval_ms, 17)
            self.assertEqual(args.keepalives, 3)
            self.assertAlmostEqual(args.timeout, 15.051)

    def test_zero_negative_fractional_and_overflowing_intervals_are_rejected(self):
        for option, value in (('--interval-ms', '0'), ('--interval-ms', '-1'),
                              ('--interval-ms', '1.5'), ('--interval-ms', '2147483648'),
                              ('--iterations', '-1'), ('--iterations', '2147483648')):
            with self.subTest(option=option, value=value), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                proof.main(['--home-base', '/unused', option, value])
            self.assertEqual(error.exception.code, 2)

    def test_masked_cli_frames_split_at_every_byte(self):
        payload = keepalive(2**63 + 3)
        stream = ServerStream(client=True)
        data = b'GET / HTTP/1.1\r\n\r\n' + masked_frame(payload)
        received = []
        for byte in data:
            received.extend(stream.feed(bytes([byte])))
        self.assertEqual(received, [payload])
        ledger = KeepaliveLedger()
        ledger.observe(received[0], True, 25)
        ledger.observe(keepalive(2**63 + 3, True), False, 25.01)
        self.assertEqual(ledger.report()[0]['id'], 2**63 + 3)

    def test_two_distinct_requests_need_two_matching_acks(self):
        ledger = KeepaliveLedger()
        ledger.observe(keepalive(1), True, 25)
        ledger.observe(keepalive(2), True, 50)
        ledger.observe(keepalive(2, True), False, 50.01)
        self.assertIsNone(ledger.report()[0]['ack_at'])
        ledger.observe(keepalive(1, True), False, 50.02)
        self.assertEqual(len(ledger.acknowledged), 2)

    def test_unsolicited_duplicate_or_failed_ack_is_rejected(self):
        for status, request_id in ((200, 2), (500, 1)):
            ledger = KeepaliveLedger()
            ledger.observe(keepalive(1), True, 25)
            with self.assertRaises(ValueError):
                ledger.observe(keepalive(request_id, True, status), False, 25.1)
        ledger = KeepaliveLedger()
        ledger.observe(keepalive(1), True, 25)
        ledger.observe(keepalive(1, True), False, 25.01)
        with self.assertRaises(ValueError):
            ledger.observe(keepalive(1, True), False, 25.02)

    def test_duplicate_decreasing_or_zero_request_ids_are_rejected(self):
        for request_id in (0, 1, 2):
            ledger = KeepaliveLedger()
            ledger.observe(keepalive(2), True, 25)
            with self.assertRaises(ValueError):
                ledger.observe(keepalive(request_id), True, 50)

    def test_direction_and_masking_errors_are_rejected(self):
        with self.assertRaises(ValueError):
            KeepaliveLedger().observe(keepalive(1, True), True, 0)
        with self.assertRaises(ValueError):
            ServerStream(client=True).feed(b'GET / HTTP/1.1\r\n\r\n' + frame(keepalive(1)))
        with self.assertRaises(ValueError):
            ServerStream().feed(b'HTTP/1.1 101 OK\r\n\r\n' + masked_frame(keepalive(1, True)))


if __name__ == '__main__':
    unittest.main()
