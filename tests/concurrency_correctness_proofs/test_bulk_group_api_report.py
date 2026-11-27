import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('bulk_group_api_report', Path(__file__).with_name('bulk_group_api_report.py'))
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)


class ApiSubmissionVerdict(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / 'expected.txt').write_text('fence_a\nfence_b\n')
        self.api = self.root / 'api.log'
        self.publications(['fence_a', 'fence_b'])
        self.worker(1, 'fence_a')
        self.worker(2, 'fence_b')

    def publications(self, names):
        self.api.write_text(''.join(f"_FenceCommandIntraMarshal {{group_name:'{name}'}}: Publishing Fence intra-message...\n" for name in names))

    def worker(self, number, name, code=0, attempts=1, replies=1):
        directory = self.root / f'cli_{number}'
        directory.mkdir(exist_ok=True)
        (self.root / f'expected_{number}.txt').write_text(name + '\n')
        (directory / 'rc').write_text(str(code))
        (directory / 'ufsrvcli.log').write_text(
            f'group join: POST http://api/V1/Fence (fname={name})\n' * attempts +
            f'group join: reply for {name}: {{"needsSync":false}}\n' * replies)

    def report(self):
        return REPORT.build_report(self.root, 2, self.api)

    def test_complete_submission_has_explicit_limited_scope_pass(self):
        result = self.report()
        self.assertTrue(result['passed'])
        self.assertEqual((result['requested'], result['attempted'], result['http_accepted'], result['api_publish_records']), (2, 2, 2, 2))
        self.assertIn('Redis staging success', result['unverified'])
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            REPORT.print_report(result, self.root / 'report.json')
        self.assertIn('PASS — API submission', output.getvalue())
        self.assertIn('Redis mutations NOT VERIFIED', output.getvalue())

    def test_failed_post_names_worker_and_preserves_nonzero_exit(self):
        self.worker(1, 'fence_a', code=1, replies=0)
        self.publications(['fence_b'])
        result = self.report()
        self.assertFalse(result['passed'])
        self.assertEqual(result['exit_code'], 1)
        self.assertEqual(result['http_accepted'], 1)
        self.assertEqual(result['failed_requests'][0]['name'], 'fence_a')
        self.assertIn('HTTP successes=0', result['failed_requests'][0]['reasons'])
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            REPORT.print_report(result, self.root / 'report.json')
        self.assertIn('FAIL — API submission', output.getvalue())
        self.assertIn('name=fence_a', output.getvalue())

    def test_zero_exit_without_posts_cannot_pass(self):
        self.worker(1, 'fence_a', attempts=0, replies=0)
        result = self.report()
        self.assertFalse(result['passed'])
        self.assertEqual(result['exit_code'], 1)

    def test_http_success_without_api_publish_cannot_pass(self):
        self.publications(['fence_b'])
        self.assertFalse(self.report()['passed'])

    def test_duplicate_post_or_reply_cannot_pass(self):
        for attempts, replies in [(2, 1), (1, 2)]:
            with self.subTest(attempts=attempts, replies=replies):
                self.worker(1, 'fence_a', attempts=attempts, replies=replies)
                self.assertFalse(self.report()['passed'])

    def test_duplicate_publish_cannot_pass(self):
        self.publications(['fence_a', 'fence_a', 'fence_b'])
        self.assertFalse(self.report()['passed'])

    def test_missing_evidence_cannot_pass(self):
        (self.root / 'cli_1/rc').unlink()
        result = self.report()
        self.assertFalse(result['passed'])
        self.assertEqual(result['exit_code'], 255)

    def test_validation_error_retains_exit_two(self):
        self.worker(1, 'fence_a', code=2, attempts=0, replies=0)
        self.assertEqual(self.report()['exit_code'], 2)

    def test_reassigned_name_cannot_hide_missing_operation(self):
        self.worker(2, 'fence_a')
        result = self.report()
        self.assertFalse(result['passed'])
        self.assertIn('worker assignments do not cover each requested name exactly once', result['issues'])


if __name__ == '__main__':
    unittest.main()
