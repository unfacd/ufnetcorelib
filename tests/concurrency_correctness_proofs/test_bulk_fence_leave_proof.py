import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import sys
import time
import unittest

SCRIPT = Path(__file__).with_name('bulk_fence_leave_proof.sh').resolve()
FAKE = r'''#!/usr/bin/env python3
import fcntl, json, os, shlex, sys, time
from pathlib import Path
args=sys.argv[1:]
if '--json' in args: sys.exit(1)
state_path=Path(os.environ['LEAVE_TEST_STATE'])
if Path(sys.argv[0]).name == 'fakecli':
    home=Path(args[args.index('--home-base')+1])
    uid=str(41+int(home.name))
    print('X-UFSRVUID: '+uid, flush=True)
    option=next((a for a in args if a.startswith('--unfacd-group-leave=')),None)
    if option is None: sys.exit(0)
    ids=option.split('=',1)[1].replace('#','').split(',')
    mode=os.environ.get('LEAVE_TEST_MODE','ok')
    for fid in ids:
        print('group join: POST http://api/V1/Fence (fname=#'+fid+')',flush=True)
        print('group join: reply for #'+fid+': {"needsSync":false}',flush=True)
    selected=ids[:1] if mode in ('timeout','fail') else ids
    with open(str(state_path)+'.lock','w') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        state=json.loads(state_path.read_text())
        for fid in selected:
            state['users'][uid].remove(fid)
            state['members'][fid].remove(uid)
            state['fences'][fid]['event_counter']=str(int(state['fences'][fid]['event_counter'])+1)
            if mode in ('destroy','leaked_index'): state['fences'].pop(fid)
            if mode=='wrong_counter': state['fences'][fid]['event_counter']='999'
        state_path.write_text(json.dumps(state))
    for index,fid in enumerate(selected):
        if mode!='no_confirm':
            outcome='CREATED' if mode=='wrong_outcome' else 'ALREADY-IN'
            command,args_value=(0,7) if mode=='wrong_outcome' else (1,2)
            print(f'group join: StateSync[{index}] command={command} args={args_value} args_error=0 args_error_client=0',flush=True)
            print(f'group join: outcome[{index}] {outcome} (args=4)',flush=True)
            print(f"group join: group[{index}] fid={fid} fname='test'",flush=True)
    if mode=='timeout': time.sleep(10)
    if mode=='fail': sys.exit(2)
    sys.exit(0)
with open(str(state_path)+'.lock','w') as lock:
    fcntl.flock(lock,fcntl.LOCK_EX)
    state=json.loads(state_path.read_text())
commands=[]
filtered=[a for a in args if a not in ('--raw','--json','--csv')]
while filtered and filtered[0] in ('-h','-p'):
    filtered=filtered[2:]
if filtered: commands=[filtered]
else: commands=[shlex.split(line) for line in sys.stdin if line.strip()]
for cmd in commands:
    op,key,*rest=cmd
    if os.environ.get('LEAVE_TEST_REDIS_ERROR'):
        print('(error) WRONGTYPE test failure'); continue
    result=None
    if op=='ZRANGE':
        result=[fid+':0' for fid in state['users'].get(key.split(':')[1],[])]
    elif op=='ZSCORE':
        if key.startswith('MEMBER_USERS_FOR_FENCE:'):
            result='100' if rest[0] in state['members'].get(key.split(':')[1],[]) else None
        elif key.startswith('UF:'):
            result='100' if rest[0].split(':')[0] in state['users'].get(key.split(':')[1],[]) else None
        else:
            fid=rest[0].split(':')[-1] if key=='FENCES_MAIN' else rest[0].split(':')[0]
            if key=='FENCES_NAMEINDEX': fid=rest[0].split(':')[1]
            result='0' if fid in state['fences'] or os.environ.get('LEAVE_TEST_MODE')=='leaked_index' else None
    elif op=='ZCARD': result=len(state['members'].get(key.split(':')[1],[]))
    elif op in ('HGET','HMGET'):
        record=state['fences'].get(key.split(':')[1],{}) if key.startswith('BID:') else {}
        result=[record.get(field) for field in rest] if op=='HMGET' else record.get(rest[0])
    elif op=='EXISTS': result=int(key.split(':')[1] in state['fences'])
    elif op=='GEOPOS': result=[['0','0'] if rest[0].split(':')[0] in state['fences'] else None]
    else: raise RuntimeError(cmd)
    if '--csv' in args:
        cells=result if isinstance(result,list) else [result]
        print(','.join('NULL' if cell is None else json.dumps(cell,ensure_ascii=False) for cell in cells))
    elif isinstance(result,list):
        for value in result: print('' if value is None else value)
    else: print('' if result is None else result)
'''


class LeaveProofTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='leave-proof-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        for name in ('fakecli', 'redis-cli'):
            path = self.bin / name
            path.write_text(FAKE)
            path.chmod(0o755)
        home = self.root / 'agents' / '1'
        home.mkdir(parents=True)
        for name in ('ufsrvcli.config', 'ufsrvcli.prefs'):
            (home / name).write_text('test fixture\n')
        self.state_path = self.root / 'state.json'
        self.api_log = self.root / 'api.log'
        self.ws_log = self.root / 'websock.log'
        self.api_log.write_text('before run\n')
        self.ws_log.write_text('before run\n')
        self.seed(2)
        self.env = dict(os.environ, PATH=str(self.bin)+os.pathsep+os.environ['PATH'],
                        LEAVE_TEST_STATE=str(self.state_path))

    def seed(self, count):
        fids = [str(10000+i) for i in range(count)]
        fences = {fid:dict(id=fid, type='128', event_counter='2', cname='*:*:*:0:test'+fid,
                          dname='test'+fid, uid='42', lng='0', lat='0') for fid in fids}
        self.state_path.write_text(json.dumps(dict(users={'42':fids},
                                                   members={fid:['42'] for fid in fids}, fences=fences)))

    def command(self):
        return [str(SCRIPT), '--home-base', str(self.root/'agents'), '--account', '1',
                '--all-fences', '--local', '--cli', str(self.bin/'fakecli'),
                '--evidence-dir', str(self.root/'evidence'), '--timeout', '.4']

    def run_proof(self, mode='ok', workers=1):
        env=dict(self.env, LEAVE_TEST_MODE=mode, LOG=str(self.ws_log), APILOG=str(self.api_log))
        self.result=subprocess.run(self.command()+['--workers', str(workers)],env=env,text=True,capture_output=True,timeout=20)
        directories=list((self.root/'evidence').glob('leave_*'))
        self.assertEqual(len(directories),1,self.result.stderr)
        self.evidence=directories[0]
        report=self.evidence/'report.json'
        self.assertTrue(report.exists(),self.result.stdout+self.result.stderr)
        return json.loads(report.read_text())

    def test_timeout_preserves_partial_progress_and_report(self):
        report=self.run_proof('timeout')
        self.assertEqual(self.result.returncode,1)
        self.assertEqual(report['summary']['applied'],1)
        self.assertEqual(report['summary']['confirmed'],1)
        self.assertEqual(report['batches'][0]['status'],'TIMEOUT')
        self.assertIn('Final verification report',self.result.stdout)
        self.assertTrue((self.evidence/'run.log').exists())

    def test_applied_without_confirmation_remains_failure(self):
        report=self.run_proof('no_confirm')
        self.assertEqual(self.result.returncode,1)
        self.assertEqual(report['summary']['applied'],2)
        self.assertEqual(report['summary']['confirmed'],0)
        self.assertEqual(report['summary']['unconfirmed_applied'],2)

    def test_deleted_empty_fences_are_verified_without_counter_claim(self):
        report=self.run_proof('destroy')
        self.assertEqual(self.result.returncode,0,self.result.stdout+self.result.stderr)
        self.assertTrue(all(row['event_status']=='UNAVAILABLE_DELETED' for row in report['fences']))
        self.assertTrue(all(row['mutation_ok'] for row in report['fences']))

    def test_preflight_failure_still_has_final_report(self):
        (self.root/'agents/1/ufsrvcli.prefs').unlink()
        report=self.run_proof()
        self.assertEqual(self.result.returncode,2)
        self.assertEqual(report['status'],'FAIL')
        self.assertTrue(report['errors'])

    def test_all_batches_and_local_evidence(self):
        self.seed(18)
        report=self.run_proof()
        self.assertEqual(self.result.returncode,0,self.result.stdout+self.result.stderr)
        self.assertEqual([row['requested'] for row in report['batches']],[16,2])
        self.assertEqual(report['summary']['applied'],18)
        self.assertTrue((self.evidence/'evidence_api.log').exists())
        self.assertTrue((self.evidence/'evidence_websock.log').exists())

    def test_failure_stops_next_batch_but_checks_every_selected_fence(self):
        self.seed(18)
        report=self.run_proof('fail')
        self.assertEqual(self.result.returncode,1)
        self.assertEqual(len(report['batches']),1)
        self.assertEqual(len(report['fences']),18)
        self.assertEqual(report['summary']['applied'],1)
        self.assertEqual(report['summary']['not_submitted'],2)


    def test_deleted_fence_with_stale_search_index_fails(self):
        report=self.run_proof('leaked_index')
        self.assertEqual(self.result.returncode,1)
        self.assertTrue(all(not row['mutation_ok'] for row in report['fences']))

    def test_wrong_counter_fails_despite_removal_and_confirmation(self):
        report=self.run_proof('wrong_counter')
        self.assertEqual(self.result.returncode,1)
        self.assertEqual(report['summary']['applied'],2)
        self.assertEqual(report['summary']['confirmed'],2)
        self.assertEqual(report['summary']['mutations_verified'],0)

    def test_created_outcome_cannot_pass_as_leave_confirmation(self):
        report=self.run_proof('wrong_outcome')
        self.assertEqual(self.result.returncode,1)
        self.assertEqual(report['summary']['confirmed'],0)

    def test_empty_selection_reports_no_requests(self):
        self.seed(0)
        report=self.run_proof()
        self.assertEqual(self.result.returncode,0)
        self.assertEqual(report['summary']['selected'],0)
        self.assertIn('No leave requests sent',self.result.stdout)

    def test_redis_error_is_not_treated_as_empty_membership(self):
        self.env['LEAVE_TEST_REDIS_ERROR']='1'
        report=self.run_proof()
        self.assertEqual(self.result.returncode,2)
        self.assertTrue(report['errors'])

    def test_list_fences_prints_only_ids(self):
        command=self.command()
        command[command.index('--all-fences')]='--list-fences'
        result=subprocess.run(command,env=self.env,text=True,capture_output=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(result.stdout,'10000\n10001\n')
        self.assertFalse((self.root/'evidence').exists())

    def test_workers_share_selected_account_and_cap_to_operations(self):
        report = self.run_proof(workers=10)
        result = self.result
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertEqual(report['summary']['applied'], 2)
        self.assertEqual({row['uid'] for row in report['fences']}, {'42'})
        self.assertEqual(report['workers'], {'requested': 10, 'launched': 2})
        self.assertIn('requested=10 launched=2', result.stdout)

    def test_twenty_fences_ten_workers_all_use_one_account(self):
        self.seed(20)
        report = self.run_proof(workers=10)
        self.assertEqual(self.result.returncode, 0, self.result.stdout+self.result.stderr)
        self.assertEqual(report['summary']['applied'], 20)
        self.assertEqual(report['workers'], {'requested': 10, 'launched': 10})
        submitted = [fid for batch in report['batches'] for fid in batch['fids']]
        self.assertEqual(len(submitted), 20)
        self.assertEqual(len(set(submitted)), 20)
        self.assertEqual({row['uid'] for row in report['fences']}, {'42'})
        self.assertTrue(all(row['event_after'] == 3 for row in report['fences']))

    def test_empty_selection_launches_no_workers(self):
        self.seed(0)
        report = self.run_proof(workers=10)
        self.assertEqual(self.result.returncode, 0, self.result.stdout+self.result.stderr)
        self.assertEqual(report['workers'], {'requested': 10, 'launched': 0})
        self.assertEqual(report['batches'], [])

    def assert_interrupted_report(self, signum):
        env=dict(self.env,LEAVE_TEST_MODE='timeout',LOG=str(self.ws_log),APILOG=str(self.api_log))
        command=self.command()
        command[-1]='10'
        process=subprocess.Popen(command,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            deadline=time.monotonic()+10
            while len(json.loads(self.state_path.read_text())['users']['42'])==2:
                self.assertLess(time.monotonic(),deadline)
                time.sleep(.02)
            process.send_signal(signum)
            stdout,stderr=process.communicate(timeout=5)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
        self.assertEqual(process.returncode,130,stdout+stderr)
        directory=next((self.root/'evidence').glob('leave_*'))
        report=json.loads((directory/'report.json').read_text())
        self.assertEqual(report['summary']['applied'],1)
        self.assertEqual(report['batches'][0]['status'],'INTERRUPTED')
        self.assertIn('Final verification report',stdout)

    def test_interrupt_saves_report_and_kills_cli(self):
        self.assert_interrupted_report(signal.SIGINT)

    def test_termination_saves_report_and_kills_cli(self):
        self.assert_interrupted_report(signal.SIGTERM)

    def test_sticky_fence_deletion_cannot_pass_as_orphan_cleanup(self):
        state=json.loads(self.state_path.read_text())
        for record in state['fences'].values(): record['type']=str(128+2048)
        self.state_path.write_text(json.dumps(state))
        report=self.run_proof('destroy')
        self.assertEqual(self.result.returncode,1)
        self.assertTrue(all(not row['mutation_ok'] for row in report['fences']))

    def test_redis_csv_preserves_nil_strings_and_utf8(self):
        spec=importlib.util.spec_from_file_location('leave_proof',SCRIPT.with_suffix('.py'))
        module=importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        line=r'NULL,"NULL","comma,quote\"","café","\xc3\xa9",42'
        self.assertEqual(module.redis_csv(line),[None,'NULL','comma,quote"','café','é',42])
        for bad in ('"unterminated','NULL,','ERROR,"WRONGTYPE"',r'"\q"'):
            with self.assertRaises(ValueError): module.redis_csv(bad)


if __name__ == '__main__':
    unittest.main()
