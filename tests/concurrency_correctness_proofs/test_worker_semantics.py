import ast
from concurrent.futures import ThreadPoolExecutor
from types import SimpleNamespace
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
FAKE = '''#!/usr/bin/env python3
import fcntl, json, os, sys
from pathlib import Path
args=sys.argv[1:]
home=Path(args[args.index('--home-base')+1])
option=next(a for a in args if a.startswith('--unfacd-group-join='))
(home/'invocation.json').write_text(json.dumps(dict(home=str(home), config=(home/'ufsrvcli.config').read_text(), prefs=(home/'ufsrvcli.prefs').read_text(), names=option.split('=',1)[1].split(','))))
names=option.split('=',1)[1].split(',')
(home/'ufsrvcli.log').write_text('X-UFSRVUID: 663\\n'+''.join('group join: POST http://api/V1/Fence (fname='+name+')\\ngroup join: reply for '+name+': {}\\n' for name in names))
with open(os.environ['APILOG'],'a') as api:
    fcntl.flock(api,fcntl.LOCK_EX)
    api.write(''.join("_FenceCommandIntraMarshal {group_name:'"+name+"'}: Publishing Fence intra-message...\\n" for name in names))
(home/'ufsrvcli.prefs').write_text('modified private copy')
'''


class WorkerSemantics(unittest.TestCase):
    def run_creation(self, groups, workers, clients):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefs = {}
            for account in clients:
                home = root/'profiles'/str(account)
                home.mkdir(parents=True)
                (home/'ufsrvcli.config').write_text(f'endpoint {account}')
                prefs[account] = f'cookie="cookie-{account}"; ufsrvuid="{660+account}"'
                (home/'ufsrvcli.prefs').write_text(prefs[account])
            cli = root/'ufsrvcli'
            cli.write_text(FAKE)
            cli.chmod(0o700)
            api = root/'api.log'
            api.touch()
            evidence = root/'evidence'
            env = dict(os.environ, EVIDENCE_DIR=str(evidence), APILOG=str(api))
            command = [str(ROOT/'bulk_group_creation_proof.sh'), '--local', '--api-only',
                       '--cli', str(cli), '--home-base', str(root/'profiles'),
                       '--account', ','.join(str(account) for account in clients),
                       '--groups', str(groups)]
            if workers is not None:
                command += ['--workers', str(workers)]
            result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            invocations = [json.loads(path.read_text()) for path in evidence.rglob('invocation.json')]
            self.assertEqual(len(invocations), len(clients))
            names = [name for invocation in invocations for name in invocation['names']]
            self.assertEqual(len(names), groups)
            self.assertEqual(len(set(names)), groups)
            seen = {}
            for invocation in invocations:
                home = Path(invocation['home'])
                account = int(home.name)
                self.assertIn(account, clients)
                self.assertNotIn(account, seen, 'one client signed on more than once')
                seen[account] = invocation['names']
                self.assertEqual(invocation['config'], f'endpoint {account}')
                self.assertEqual(invocation['prefs'], prefs[account])
                self.assertEqual((root/'profiles'/str(account)/'ufsrvcli.prefs').read_text(),
                                 prefs[account], 'the source home was modified')
            self.assertEqual(sorted(seen), sorted(clients))
            return result

    def test_each_client_creates_its_own_share(self):
        result = self.run_creation(6, None, [1, 2, 3])
        self.assertIn('requested=3 launched=3', result.stdout)

    def test_fewer_workers_than_clients_share_them_out(self):
        result = self.run_creation(8, 2, [1, 2, 3, 4])
        self.assertIn('requested=2 launched=4', result.stdout)

    def test_more_workers_than_clients_gives_one_each(self):
        result = self.run_creation(4, 10, [1, 2])
        self.assertIn('requested=10 launched=2', result.stdout)
        self.assertIn('Worker count reduced', result.stdout)

    def test_single_client_single_worker_keeps_the_whole_list(self):
        result = self.run_creation(3, None, [1])
        self.assertIn('requested=1 launched=1', result.stdout)

    def test_single_client_with_several_workers_needs_the_full_proof(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root/'profiles'/'1'
            home.mkdir(parents=True)
            (home/'ufsrvcli.config').write_text('endpoint 1')
            (home/'ufsrvcli.prefs').write_text('cookie="one"; ufsrvuid="661"')
            cli = root/'ufsrvcli'
            cli.write_text(FAKE)
            cli.chmod(0o700)
            api = root/'api.log'
            api.touch()
            result = subprocess.run([str(ROOT/'bulk_group_creation_proof.sh'), '--local', '--api-only',
                                     '--cli', str(cli), '--home-base', str(root/'profiles'),
                                     '--account', '1', '--groups', '3', '--workers', '3'],
                                    env=dict(os.environ, EVIDENCE_DIR=str(root/'evidence'), APILOG=str(api)),
                                    text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 2, result.stdout+result.stderr)
            self.assertIn('--api-only cannot exercise refused sign-ons', result.stderr)

    def test_two_clients_sharing_one_cookie_are_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for account in (1, 2):
                home = root/'profiles'/str(account)
                home.mkdir(parents=True)
                (home/'ufsrvcli.config').write_text(f'endpoint {account}')
                (home/'ufsrvcli.prefs').write_text('cookie="shared"; ufsrvuid="661"')
            cli = root/'ufsrvcli'
            cli.write_text(FAKE)
            cli.chmod(0o700)
            api = root/'api.log'
            api.touch()
            result = subprocess.run([str(ROOT/'bulk_group_creation_proof.sh'), '--local', '--api-only',
                                     '--cli', str(cli), '--home-base', str(root/'profiles'),
                                     '--account', '1,2', '--groups', '4'],
                                    env=dict(os.environ, EVIDENCE_DIR=str(root/'evidence'), APILOG=str(api)),
                                    text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 2, result.stdout+result.stderr)
            self.assertIn('one session cookie', result.stderr)

    def test_message_participants_and_workers_are_independent(self):
        source = (ROOT/'group_message_delivery_proof.sh').read_text().split("<<'PY'\n", 1)[1].rsplit('\nPY', 1)[0]
        tree = ast.parse(source)
        phase = next(node for node in tree.body if isinstance(node, ast.Try))
        selected = []
        for node in phase.body:
            if isinstance(node, ast.Assign) and any(isinstance(target, ast.Name) and target.id in
                                                   ('accounts', 'operations', 'flat_results', 'messages')
                                                   for target in node.targets):
                selected.append(node)
            if isinstance(node, ast.With) and any(isinstance(child, ast.Name) and child.id == 'flat_results'
                                                 for child in ast.walk(node)):
                selected.append(node)
        sends = []
        namespace = dict(args=SimpleNamespace(account=7, agents=2, messages=2), worker_count=4,
                         total=4, ThreadPoolExecutor=ThreadPoolExecutor,
                         send_message=lambda operation: sends.append(operation) or operation)
        exec(compile(ast.Module(body=selected, type_ignores=[]), '<message scheduling>', 'exec'), namespace)
        self.assertEqual(namespace['accounts'], [7, 8])
        self.assertEqual(sorted(sends), [(7, 1), (7, 2), (8, 1), (8, 2)])
        self.assertEqual(namespace['messages'], [[(7, 1), (7, 2)], [(8, 1), (8, 2)]])

    def test_connection_proofs_schedule_operations_without_adding_accounts(self):
        for script, accounts, workers, iterations in (('multi_user_proof.sh', 2, 3, 2),
                                                       ('rogue_client_proof.sh', 1, 3, 2)):
            with self.subTest(script=script), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                for account in range(1, accounts+1):
                    home = root/'profiles'/str(account)
                    home.mkdir(parents=True)
                    (home/'ufsrvcli.config').write_text(f'endpoint {account}')
                    (home/'ufsrvcli.prefs').write_text(f'identity {account}')
                cli = root/'fakecli'
                cli.write_text("#!/usr/bin/env python3\nimport sys\nfrom pathlib import Path\na=sys.argv[1:]\nh=Path(a[a.index('--home-base')+1])\nwith (h/'calls.log').open('a') as f: f.write((h/'ufsrvcli.prefs').read_text()+'\\n')\n")
                cli.chmod(0o700)
                source = (ROOT/script).read_text()
                start = source.index('RUN_HOME=$(mktemp')
                end = source.index('# ── 4.', start)
                operations = accounts*iterations if script.startswith('multi') else workers*iterations
                prelude = f'DELAYS=({' '.join(['2']*operations)})\nwait_quiescent() {{ :; }}\n'
                env = dict(os.environ, EVIDENCE_DIR=str(root), CLI=str(cli), PROFILES_DIR=str(root/'profiles'),
                           ACCOUNT='1', ACCOUNTS=str(accounts), WORKERS=str(workers), ITER=str(iterations),
                           C=str(operations), SEED='1')
                result = subprocess.run(['bash', '-u', '-c', prelude+source[start:end]], env=env,
                                        text=True, capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                calls = [line for path in root.rglob('calls.log') for line in path.read_text().splitlines()]
                self.assertEqual(len(calls), operations)
                for account in range(1, accounts+1):
                    expected = iterations if accounts > 1 else operations
                    self.assertEqual(calls.count(f'identity {account}'), expected)

    def test_all_cli_proofs_expose_workers(self):
        for name in ('bulk_group_creation_proof.sh', 'bulk_fence_leave_proof.sh',
                     'group_message_delivery_proof.sh', 'multi_user_proof.sh', 'rogue_client_proof.sh'):
            with self.subTest(script=name):
                result = subprocess.run([str(ROOT/name), '--help'], text=True, capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('--workers', result.stdout)
                self.assertNotIn('--clients', result.stdout)


if __name__ == '__main__':
    unittest.main()
