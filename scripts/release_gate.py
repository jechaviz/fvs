#!/usr/bin/env python3
from __future__ import annotations
import argparse, json, pathlib, subprocess, sys, time, urllib.error, urllib.request


def get_json(url:str, *, headers=None, timeout=5):
    req=urllib.request.Request(url, headers=headers or {})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        raw=r.read(1024*1024)
        return r.status, json.loads(raw.decode('utf-8'))

def get_text(url:str, *, headers=None, timeout=5):
    req=urllib.request.Request(url, headers=headers or {})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read(1024*1024).decode('utf-8','replace')


def main()->int:
    ap=argparse.ArgumentParser(description='FVS blue/green promotion gate')
    ap.add_argument('base_url')
    ap.add_argument('--admin-key', required=True)
    ap.add_argument('--expected-core-prefix', default='6.0.0')
    ap.add_argument('--samples', type=int, default=3)
    ap.add_argument('--interval', type=float, default=1.0)
    ap.add_argument('--allow-manual-review', type=int, default=0)
    ap.add_argument('--min-workers', type=int, default=1)
    ap.add_argument('--backup', type=pathlib.Path, help='Require this verified logical backup before promotion')
    ap.add_argument('--backup-max-age-hours', type=float, default=26.0)
    ap.add_argument('--required-schema', default='001_current.sql')
    args=ap.parse_args()
    base=args.base_url.rstrip('/')
    if args.backup:
        verifier=pathlib.Path(__file__).with_name('verify_backup.py')
        cp=subprocess.run([sys.executable,str(verifier),str(args.backup),'--max-age-hours',str(args.backup_max_age_hours),'--required-schema',args.required_schema],capture_output=True,text=True)
        if cp.returncode != 0:
            detail=(cp.stdout or cp.stderr).strip()
            print(f'FAIL backup verification: {detail}',file=sys.stderr)
            return 2
    headers={'X-Admin-Key':args.admin_key,'User-Agent':'FVS-release-gate/6.0.0'}
    problems=[]
    for i in range(max(1,args.samples)):
        try:
            _, live=get_json(base+'/api/v1/health/live')
            if not live.get('ok'): problems.append('liveness=false')
            core=str(live.get('core') or '')
            if args.expected_core_prefix and not core.startswith(args.expected_core_prefix):
                problems.append(f'core version mismatch: {core!r}')
            _, ready=get_json(base+'/api/v1/health/ready')
            if not ready.get('ready'): problems.append('readiness=false')
            _, ops=get_json(base+'/api/v1/admin/ops',headers=headers)
            if ops.get('status')!='ok': problems.append(f"ops status={ops.get('status')!r}")
            if int(ops.get('outbox_dead',0))>0: problems.append('outbox_dead>0')
            if int(ops.get('outbox_processing_stale',0))>0: problems.append('outbox_processing_stale>0')
            if int(ops.get('webhook_stale',0))>0: problems.append('webhook_stale>0')
            if int(ops.get('workers_stale',0))>0: problems.append('workers_stale>0')
            if int(ops.get('workers',0))<args.min_workers: problems.append(f"workers={int(ops.get('workers',0))}<{args.min_workers}")
            mr=int(ops.get('manual_review_carts',0))+int(ops.get('manual_review_payments',0))
            if mr>args.allow_manual_review: problems.append(f'manual_review={mr}>{args.allow_manual_review}')
            _, metrics=get_text(base+'/api/v1/admin/metrics',headers=headers)
            if 'fvs_ops_degraded 0' not in metrics: problems.append('metrics report degraded ops')
            for required_metric in ('fvs_outbox_dead','fvs_workers_stale','fvs_manual_review_payments'):
                if required_metric not in metrics: problems.append(f'metric missing: {required_metric}')
        except urllib.error.HTTPError as e:
            problems.append(f'HTTP {e.code} at {e.url}')
        except Exception as e:
            problems.append(f'{type(e).__name__}: {e}')
        if i+1<max(1,args.samples): time.sleep(max(0.0,args.interval))
    if problems:
        for p in sorted(set(problems)): print('FAIL',p,file=sys.stderr)
        return 2
    print('PASS release gate')
    return 0

if __name__=='__main__': raise SystemExit(main())
