import unittest

from message_proof_checks import intra_requests_processed


def processed(target, consumed, topic='MSG'):
    kind = 'TARGET' if target == consumed else 'OTHER'
    return (f"sHandleStagedEncodedIntraMessage (channel_name:'UFSRV:INTRA:{topic}', "
            f"reqid_target:'{target}', reqid_processed:'{consumed}'): NOTICE: PROCESSED {kind} REQID\n")


class IntraRequestChecks(unittest.TestCase):
    def test_cross_worker_processing_counts_consumed_ids(self):
        log = processed('634', '634') + processed('635', '635') + processed('637', '636') + processed('637', '637')
        self.assertTrue(intra_requests_processed(['634', '635', '636', '637'], log, 'MSG', 4))

    def test_different_notification_can_process_every_selected_request(self):
        self.assertTrue(intra_requests_processed(['1', '2'], processed('9', '1') + processed('9', '2'), 'MSG', 2))

    def test_missing_consumed_id_fails(self):
        self.assertFalse(intra_requests_processed(['1', '2'], processed('2', '2'), 'MSG', 2))

    def test_duplicate_consumption_fails_across_both_notice_types(self):
        log = processed('1', '1') + processed('2', '1') + processed('2', '2')
        self.assertFalse(intra_requests_processed(['1', '2'], log, 'MSG', 2))

    def test_duplicate_api_ids_fail(self):
        self.assertFalse(intra_requests_processed(['1', '1'], processed('1', '1'), 'MSG', 2))

    def test_unrelated_requests_are_ignored(self):
        self.assertTrue(intra_requests_processed(['1'], processed('1', '1') + processed('9', '9'), 'MSG', 1))

    def test_wrong_topic_cannot_satisfy_processing(self):
        self.assertFalse(intra_requests_processed(['1'], processed('1', '1', 'FENCE'), 'MSG', 1))

    def test_joins_support_cross_worker_processing(self):
        self.assertTrue(intra_requests_processed(['1'], processed('2', '1', 'FENCE'), 'FENCE', 1))

    def test_empty_evidence_cannot_pass(self):
        self.assertFalse(intra_requests_processed([], '', 'MSG', 0))

    def test_target_id_alone_cannot_satisfy_processing(self):
        self.assertFalse(intra_requests_processed(['1'], processed('1', '9'), 'MSG', 1))


if __name__ == '__main__':
    unittest.main()
