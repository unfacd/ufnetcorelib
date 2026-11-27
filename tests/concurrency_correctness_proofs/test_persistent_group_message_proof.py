import base64
import unittest
from persistent_group_message_proof import DeliveryLedger


class LedgerTests(unittest.TestCase):
    def setUp(self):
        self.ledger = DeliveryLedger({1: 663, 2: 664, 3: 665}, 2, 'run')
        for account, uid in self.ledger.identities.items():
            self.ledger.observe(account, dict(event='ready', uid=str(uid), cid=str(9007199254740993 + account)))
            self.ledger.observe(account, self.event(account, 'joined', id=f'join_{account}', fid='1649137658552652065'))

    def event(self, account, kind, **extra):
        return dict(event=kind, uid=str(self.ledger.identities[account]),
                    cid=str(self.ledger.ready[account]), **extra)

    def ack(self, sender, ordinal=1, gid=9007199254740999):
        tag = f'run:a{sender}:m{ordinal}'
        self.ledger.observe(sender, self.event(sender, 'ack', id=tag, gid=str(gid), when_client=str(ordinal), fid=str(self.ledger.fid)))
        return tag

    def message(self, recipient, sender, ordinal=1, gid=9007199254740999):
        tag = f'run:a{sender}:m{ordinal}'
        return self.event(recipient, 'message', sender_uid=str(self.ledger.identities[sender]),
                          fid=str(self.ledger.fid), gid=str(gid),
                          body_base64=base64.b64encode(self.ledger.payloads[tag][1]).decode(), protocol=1, outcome='SYNCED')

    def test_every_peer_counts_and_exact_gid_is_preserved(self):
        gid = 9007199254740999
        for sender in self.ledger.identities:
            for ordinal in range(1, 3):
                self.ack(sender, ordinal, gid)
                for recipient in self.ledger.identities:
                    self.ledger.observe(recipient, self.message(recipient, sender, ordinal, gid))
                gid += 1
        self.ledger.verify()
        self.assertEqual(self.ledger.summary()['peer_deliveries'], 12)
        self.assertEqual(self.ledger.summary()['self_echoes'], 6)

    def test_duplicate_peer_delivery_fails(self):
        event = self.message(2, 1)
        self.ledger.observe(2, event)
        with self.assertRaisesRegex(ValueError, 'duplicate receiver'):
            self.ledger.observe(2, event)

    def test_wrong_sender_or_payload_fails(self):
        event = self.message(2, 1); event['sender_uid'] = '665'
        with self.assertRaisesRegex(ValueError, 'wrong sender'): self.ledger.observe(2, event)
        event = self.message(2, 1); event['body_base64'] = base64.b64encode(b'corrupted').decode()
        with self.assertRaisesRegex(ValueError, 'corrupted'): self.ledger.observe(2, event)

    def test_test_payload_on_wrong_fence_fails(self):
        event = self.message(2, 1); event['fid'] = '123'
        with self.assertRaisesRegex(ValueError, 'wrong fence'): self.ledger.observe(2, event)

    def test_duplicate_ack_and_gid_reuse_fail(self):
        self.ack(1)
        with self.assertRaisesRegex(ValueError, 'duplicate sender'): self.ack(1)
        with self.assertRaisesRegex(ValueError, 'share one message'): self.ack(2)

    def test_missing_receivers_cannot_be_replaced_by_self_echoes(self):
        self.ack(1)
        self.ledger.observe(1, self.message(1, 1))
        self.assertEqual(len(self.ledger.missing()), 12)
        with self.assertRaisesRegex(ValueError, 'incomplete'): self.ledger.verify()

    def test_unmatched_ack_and_wrong_protocol_cannot_count_as_delivery(self):
        with self.assertRaisesRegex(ValueError, 'unmatched acknowledgement'):
            self.ledger.observe(1, self.event(1, 'ack_unmatched', fid=str(self.ledger.fid)))
        event = self.message(2, 1); event['protocol'] = 0
        with self.assertRaisesRegex(ValueError, 'protocol or outcome'):
            self.ledger.observe(2, event)

    def test_connection_identity_change_fails(self):
        event = self.message(2, 1); event['cid'] = '123'
        with self.assertRaisesRegex(ValueError, 'identity changed'): self.ledger.observe(2, event)

    def test_old_staged_message_outside_test_fence_is_not_counted(self):
        event = self.message(2, 1); event['body_base64'] = base64.b64encode(b'old payload').decode(); event['fid'] = '123'
        self.ledger.observe(2, event)
        self.assertEqual(self.ledger.outside_scope, 1)
        self.assertFalse(self.ledger.received)


if __name__ == '__main__': unittest.main()
