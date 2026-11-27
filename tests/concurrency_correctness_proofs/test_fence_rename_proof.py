import unittest

from fence_rename_proof import RenameLedger


class RenameLedgerTests(unittest.TestCase):
    def setUp(self):
        self.identities = {1: 663, 2: 664, 3: 665}
        self.join_name = 'fnameproof_tag_multi'
        self.new_name = f'{self.join_name}_r'
        self.fid = '1649137658552652065'
        self.ledger = RenameLedger(dict(self.identities), self.join_name, 'rename_1', self.new_name, 1)
        for account, uid in self.identities.items():
            self.ledger.observe(account, dict(event='ready', uid=str(uid), cid=str(9007199254740993 + account)))
            self.ledger.observe(account, self.event(account, 'joined', id=f'join_{account}', fid=self.fid,
                                                    name=self.join_name))

    def event(self, account, kind, **extra):
        return dict(event=kind, uid=str(self.identities[account]),
                    cid=str(9007199254740993 + account), **extra)

    def rename(self, account=1, **extra):
        fields = dict(id='rename_1', fid=self.fid, name=self.new_name, outcome='ACCEPTED',
                      args_error=0, when_client='1700000000123', args=9)
        fields.update(extra)
        self.ledger.observe(account, self.event(account, 'renamed', **fields))

    def push(self, account, ledger=None, **extra):
        fields = dict(op=4, fid=self.fid, name=self.new_name, outcome='UPDATED',
                      args_error=0, when_client='1700000000123', args=9)
        fields.update(extra)
        (ledger or self.ledger).observe(account, self.event(account, 'fence', **fields))

    def test_single_member_confirmation_suffices(self):
        ledger = RenameLedger({1: 663}, 'fnameproof_tag_single', 'rename_1', 'fnameproof_tag_single_r', 1)
        ledger.observe(1, dict(event='ready', uid='663', cid='7000'))
        ledger.observe(1, dict(event='joined', uid='663', cid='7000', id='join_1',
                               fid=self.fid, name='fnameproof_tag_single'))
        ledger.observe(1, dict(event='renamed', uid='663', cid='7000', id='rename_1', fid=self.fid,
                               name='fnameproof_tag_single_r', outcome='ACCEPTED', args_error=0,
                               when_client='1700000000123', args=9))
        ledger.verify()
        self.assertEqual(len(ledger.renamed), 1)
        self.assertEqual(ledger.pushes, {})

    def test_renamer_and_every_other_member_confirm_once(self):
        self.rename()
        self.push(2)
        self.push(3)
        self.ledger.verify()
        self.assertEqual(sorted(self.ledger.pushes), [2, 3])
        self.assertEqual(self.ledger.summary()['members'], 3)

    def test_duplicate_push_fails(self):
        self.rename()
        self.push(2)
        with self.assertRaisesRegex(ValueError, 'duplicate name change'):
            self.push(2)

    def test_member_cannot_be_both_pulled_and_pushed(self):
        self.rename()
        with self.assertRaisesRegex(ValueError, 'both pulled and pushed'):
            self.push(1)

    def test_wrong_title_or_fence_fails(self):
        self.rename()
        with self.assertRaisesRegex(ValueError, 'another fence or title'):
            self.push(2, name='fnameproof_other')
        with self.assertRaisesRegex(ValueError, 'another fence or title'):
            self.push(2, fid='1649137658552652000')

    def test_refused_rename_is_not_a_confirmation(self):
        with self.assertRaisesRegex(ValueError, 'own copy'):
            self.rename(outcome='REJECTED', args_error=4)
        with self.assertRaisesRegex(ValueError, 'did not apply'):
            self.push(2, outcome='REJECTED')

    def test_a_fan_out_cannot_retire_a_pending_rename(self):
        # the copy the server fans out to the other members is UPDATED, not the originator's ACCEPTED
        with self.assertRaisesRegex(ValueError, 'own copy'):
            self.rename(outcome='UPDATED')

    def test_client_stamps_must_agree(self):
        self.rename()
        with self.assertRaisesRegex(ValueError, 'another client stamp'):
            self.push(2, when_client='1700000000999')

    def test_missing_member_fails_verification(self):
        self.rename()
        self.push(2)
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            self.ledger.verify()

    # ── the same-name refusal ────────────────────────────────────────────────

    def refused(self):
        ledger = RenameLedger(dict(self.identities), self.join_name, 'rename_1', self.join_name, 1,
                              expect_applied=False)
        for account, uid in self.identities.items():
            ledger.observe(account, dict(event='ready', uid=str(uid), cid=str(9007199254740993 + account)))
            ledger.observe(account, dict(event='joined', uid=str(uid), cid=str(9007199254740993 + account),
                                         id=f'join_{account}', fid=self.fid, name=self.join_name))
        return ledger

    def refusal(self, account=1, **extra):
        fields = dict(id='rename_1', fid=self.fid, name=self.join_name, outcome='REJECTED',
                      args_error=18, when_client='1700000000123', args=6)
        fields.update(extra)
        return self.event(account, 'rename_refused', **fields)

    def test_refusal_is_the_expected_confirmation(self):
        ledger = self.refused()
        ledger.observe(1, self.refusal())
        ledger.verify()
        self.assertEqual(list(ledger.refused), [1])
        self.assertEqual(ledger.renamed, {})
        self.assertEqual(ledger.pushes, {})
        self.assertEqual(ledger.summary()['refused'][0]['args_error'], 18)

    def test_refusal_must_carry_the_identical_name_reason(self):
        ledger = self.refused()
        with self.assertRaisesRegex(ValueError, 'another reason'):
            ledger.observe(1, self.refusal(args_error=1))

    def test_refusal_must_carry_a_rejected_outcome(self):
        ledger = self.refused()
        with self.assertRaisesRegex(ValueError, 'rejected outcome'):
            ledger.observe(1, self.refusal(outcome='UPDATED'))

    def test_refusal_must_name_the_title_that_was_already_set(self):
        ledger = self.refused()
        with self.assertRaisesRegex(ValueError, 'another title'):
            ledger.observe(1, self.refusal(name='fnameproof_other'))

    def test_duplicate_refusal_fails(self):
        ledger = self.refused()
        ledger.observe(1, self.refusal())
        with self.assertRaisesRegex(ValueError, 'duplicate rename refusal'):
            ledger.observe(1, self.refusal())

    def test_refusal_must_come_from_the_renamer(self):
        ledger = self.refused()
        with self.assertRaisesRegex(ValueError, 'another member'):
            ledger.observe(2, self.refusal(account=2))

    def test_missing_refusal_fails_verification(self):
        ledger = self.refused()
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            ledger.verify()

    def test_applied_rename_cannot_satisfy_an_expected_refusal(self):
        ledger = self.refused()
        with self.assertRaisesRegex(ValueError, 'had to be refused'):
            ledger.observe(1, self.event(1, 'renamed', id='rename_1', fid=self.fid, name=self.join_name,
                                         outcome='ACCEPTED', args_error=0, when_client='1700000000123', args=9))

    def test_change_delivered_to_a_member_fails_an_expected_refusal(self):
        ledger = self.refused()
        with self.assertRaisesRegex(ValueError, 'delivered to a member'):
            self.push(2, ledger)

    def test_applied_mode_rejects_a_refusal(self):
        with self.assertRaisesRegex(ValueError, 'rename refused'):
            self.ledger.observe(1, self.refusal())

    # ── concurrent renames ──────────────────────────────────────────────────

    def concurrent(self):
        ledger = RenameLedger(dict(self.identities), self.join_name, 'rename_1', self.new_name, 1,
                              concurrent=True)
        for account, uid in self.identities.items():
            ledger.observe(account, dict(event='ready', uid=str(uid), cid=str(9007199254740993 + account)))
            ledger.observe(account, dict(event='joined', uid=str(uid), cid=str(9007199254740993 + account),
                                         id=f'join_{account}', fid=self.fid, name=self.join_name))
        return ledger

    def concurrent_refusal(self, account, **extra):
        # a non-owner proposal is refused for ownership before the name is ever compared
        fields = dict(id=f'rename_{account}', fid=self.fid, name=self.new_name, outcome='REJECTED',
                      args_error=3, when_client='1700000000123', args=6)
        fields.update(extra)
        return self.event(account, 'rename_refused', **fields)

    def concurrent_applied(self, account, **extra):
        fields = dict(id=f'rename_{account}', fid=self.fid, name=self.new_name, outcome='ACCEPTED',
                      args_error=0, when_client='1700000000123', args=9)
        fields.update(extra)
        return self.event(account, 'renamed', **fields)

    def test_concurrent_renames_settle_on_one_winner(self):
        ledger = self.concurrent()
        ledger.observe(1, self.concurrent_applied(1))
        ledger.observe(2, self.concurrent_refusal(2))
        ledger.observe(3, self.concurrent_refusal(3))
        self.push(2, ledger)
        self.push(3, ledger)
        ledger.verify()
        self.assertEqual(list(ledger.renamed), [1])
        self.assertEqual(sorted(ledger.refused), [2, 3])
        self.assertEqual(sorted(ledger.pushes), [2, 3])

    def test_two_concurrent_winners_fail(self):
        ledger = self.concurrent()
        ledger.observe(1, self.concurrent_applied(1))
        with self.assertRaisesRegex(ValueError, 'a second member applied'):
            ledger.observe(2, self.concurrent_applied(2))

    def test_a_member_cannot_both_win_and_be_refused(self):
        ledger = self.concurrent()
        ledger.observe(2, self.concurrent_refusal(2))
        with self.assertRaisesRegex(ValueError, 'both refused and applied'):
            ledger.observe(2, self.concurrent_applied(2))

    def test_missing_concurrent_answer_fails_verification(self):
        ledger = self.concurrent()
        ledger.observe(1, self.concurrent_applied(1))
        ledger.observe(2, self.concurrent_refusal(2))
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            ledger.verify()

    def test_concurrent_refusal_must_match_its_request_id(self):
        ledger = self.concurrent()
        with self.assertRaisesRegex(ValueError, 'correlation mismatch'):
            ledger.observe(2, self.concurrent_refusal(2, id='rename_1'))

    def test_only_the_owner_may_win(self):
        ledger = self.concurrent()
        ledger.observe(2, self.concurrent_applied(2))
        ledger.observe(1, self.concurrent_refusal(1))
        ledger.observe(3, self.concurrent_refusal(3))
        with self.assertRaisesRegex(ValueError, 'owner must be the one'):
            ledger.verify()

    def test_a_concurrent_refusal_must_not_be_the_identical_name_error(self):
        ledger = self.concurrent()
        with self.assertRaisesRegex(ValueError, 'another reason'):
            ledger.observe(2, self.concurrent_refusal(2, args_error=18))

    def test_the_winner_must_not_receive_the_change_as_a_push(self):
        ledger = self.concurrent()
        self.push(1, ledger)
        ledger.observe(1, self.concurrent_applied(1))
        ledger.observe(2, self.concurrent_refusal(2))
        ledger.observe(3, self.concurrent_refusal(3))
        with self.assertRaisesRegex(ValueError, 'must not be pushed'):
            ledger.verify()

    def test_rename_must_be_confirmed_by_the_member_that_issued_it(self):
        ledger = RenameLedger({1: 663, 2: 664}, self.join_name, 'rename_1', self.new_name, 1)
        for account, uid in ((1, 663), (2, 664)):
            ledger.observe(account, dict(event='ready', uid=str(uid), cid=f'9{account}'))
            ledger.observe(account, dict(event='joined', uid=str(uid), cid=f'9{account}', id=f'join_{account}',
                                         fid=self.fid, name=self.join_name))
        ledger.observe(2, dict(event='renamed', uid='664', cid='92', id='rename_1', fid=self.fid,
                               name=self.new_name, outcome='ACCEPTED', args_error=0,
                               when_client='1700000000123', args=9))
        ledger.observe(1, dict(event='fence', uid='663', cid='91', op=4, fid=self.fid, name=self.new_name,
                               outcome='UPDATED', args_error=0, when_client='1700000000123', args=9))
        with self.assertRaisesRegex(ValueError, 'not confirmed by the member that issued it'):
            ledger.verify()


if __name__ == '__main__':
    unittest.main()
