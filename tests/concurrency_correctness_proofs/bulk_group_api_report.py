import argparse
from collections import Counter
import json
from pathlib import Path
import re


def build_report(workdir, workers, api_evidence):
    workdir = Path(workdir)
    expected = (workdir / 'expected.txt').read_text().splitlines()
    wanted = set(expected)
    evidence = Path(api_evidence).read_text(errors='replace')
    publications = Counter(re.findall(
        r"_FenceCommandIntraMarshal .*group_name:'([^']+)'[^\n]*Publishing Fence intra-message", evidence))
    rows, failed, issues, assignments = [], [], [], []
    first_error = 0
    for worker in range(1, workers + 1):
        directory = workdir / f'cli_{worker}'
        try:
            names = (workdir / f'expected_{worker}.txt').read_text().splitlines()
            text = (directory / 'ufsrvcli.log').read_text(errors='replace')
            code = int((directory / 'rc').read_text().strip())
        except (OSError, ValueError) as error:
            names, text, code = [], '', 255
            issues.append(f'worker {worker}: missing or invalid evidence ({type(error).__name__})')
        assignments.extend(names)
        if code and not first_error:
            first_error = code if 0 < code < 256 else 1
        posted = Counter(re.findall(r'group join: POST \S+ \(fname=([^\n]+)\)', text))
        accepted = Counter(re.findall(r'group join: reply for (.+?): ', text))
        for name in sorted((set(posted) | set(accepted)) - set(names)):
            issues.append(f'worker {worker}: unexpected name {name}')
        worker_failures = []
        for name in names:
            reasons = []
            if posted[name] != 1:
                reasons.append(f'POST attempts={posted[name]}')
            if accepted[name] != 1:
                reasons.append(f'HTTP successes={accepted[name]}')
            if publications[name] != 1:
                reasons.append(f'API publish records={publications[name]}')
            if reasons:
                item = dict(worker=worker, name=name, reasons=reasons)
                failed.append(item)
                worker_failures.append(item)
        rows.append(dict(worker=worker, requested=len(names), attempted=sum(posted[n] for n in names),
                         http_accepted=sum(accepted[n] for n in names),
                         api_publish_records=sum(publications[n] for n in names),
                         exit_code=code, passed=not code and not worker_failures))
    if Counter(assignments) != Counter(expected) or len(expected) != len(wanted):
        issues.append('worker assignments do not cover each requested name exactly once')
    passed = not first_error and not failed and not issues
    return dict(passed=passed, scope='API submission and publish-log corroboration only',
                requested=len(expected), attempted=sum(r['attempted'] for r in rows),
                http_accepted=sum(r['http_accepted'] for r in rows),
                api_publish_records=sum(publications[n] for n in expected),
                workers=rows, failed_requests=failed, issues=issues,
                unverified=['Redis staging success', 'WebSocket confirmation', 'fence creation and Redis mutations'],
                exit_code=0 if passed else first_error or 1)


def print_report(report, output):
    print('  scope: API submission only; fence creation and Redis mutations NOT VERIFIED')
    print('  worker  requested  attempted  HTTP-ok  API-publish-log  exit  verdict')
    for row in report['workers']:
        print(f"  {row['worker']:>6}  {row['requested']:>9}  {row['attempted']:>9}  "
              f"{row['http_accepted']:>7}  {row['api_publish_records']:>15}  "
              f"{row['exit_code']:>4}  {'PASS' if row['passed'] else 'FAIL'}")
    print(f"  requested={report['requested']} attempted={report['attempted']} "
          f"http_accepted={report['http_accepted']} api_publish_records={report['api_publish_records']}")
    for item in report['failed_requests']:
        print(f"  FAIL worker={item['worker']} name={item['name']}: {', '.join(item['reasons'])}")
    for issue in report['issues']:
        print(f'  FAIL: {issue}')
    print(f"  {'PASS' if report['passed'] else 'FAIL'} — API submission; "
          f"exit={report['exit_code']}; report={output}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('workdir', type=Path)
    parser.add_argument('workers', type=int)
    parser.add_argument('api_evidence', type=Path)
    args = parser.parse_args()
    output = args.workdir / 'api_submission_report.json'
    try:
        report = build_report(args.workdir, args.workers, args.api_evidence)
    except (OSError, ValueError) as error:
        print(f'  FAIL — API submission evidence could not be read: {error}')
        return 1
    output.write_text(json.dumps(report, indent=2) + '\n')
    print_report(report, output)
    return report['exit_code']


if __name__ == '__main__':
    raise SystemExit(main())
