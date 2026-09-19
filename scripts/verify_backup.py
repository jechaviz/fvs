#!/usr/bin/env python3
from __future__ import annotations
import argparse, datetime as dt, gzip, hashlib, json, pathlib, re, sys


def parse_meta(path: pathlib.Path) -> dict[str,str]:
    meta={}
    for line in path.read_text(encoding='utf-8').splitlines():
        if '=' in line:
            k,v=line.split('=',1);meta[k.strip()]=v.strip()
    return meta


def sha256_file(path: pathlib.Path) -> str:
    h=hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''): h.update(chunk)
    return h.hexdigest()


def main()->int:
    ap=argparse.ArgumentParser(description='Verify an FVS logical MySQL backup before migration/restore')
    ap.add_argument('backup',type=pathlib.Path)
    ap.add_argument('--max-age-hours',type=float,default=26.0)
    ap.add_argument('--required-schema',default='')
    args=ap.parse_args()
    b=args.backup; checksum=pathlib.Path(str(b)+'.sha256'); meta_path=pathlib.Path(str(b)+'.meta')
    problems=[]
    if not b.is_file(): problems.append('backup_missing')
    if not checksum.is_file(): problems.append('checksum_missing')
    if not meta_path.is_file(): problems.append('metadata_missing')
    if problems:
        print(json.dumps({'ok':False,'problems':problems},separators=(',',':')));return 2
    actual=sha256_file(b)
    expected=(checksum.read_text(encoding='utf-8').strip().split() or [''])[0]
    if not re.fullmatch(r'[0-9a-fA-F]{64}',expected): problems.append('checksum_file_invalid')
    elif actual.lower()!=expected.lower(): problems.append('checksum_mismatch')
    try:
        with gzip.open(b,'rb') as f:
            # Stream the entire archive to verify CRC/truncation without retaining content.
            while f.read(1024*1024): pass
    except Exception: problems.append('gzip_invalid')
    meta=parse_meta(meta_path)
    if meta.get('format')!='fvs-mysql-logical-v1': problems.append('metadata_format')
    if meta.get('archive')!=b.name: problems.append('metadata_archive')
    if meta.get('sha256','').lower()!=actual.lower(): problems.append('metadata_checksum')
    if args.required_schema and meta.get('schema_required')!=args.required_schema: problems.append('metadata_schema')
    created=meta.get('created_at_utc','')
    try:
        created_at=dt.datetime.strptime(created,'%Y%m%dT%H%M%SZ').replace(tzinfo=dt.timezone.utc)
        age_h=(dt.datetime.now(dt.timezone.utc)-created_at).total_seconds()/3600.0
        if age_h < -0.25: problems.append('backup_from_future')
        if args.max_age_hours>=0 and age_h>args.max_age_hours: problems.append('backup_stale')
    except Exception:
        age_h=None;problems.append('metadata_timestamp')
    report={'ok':not problems,'backup':str(b),'sha256':actual,'age_hours':None if age_h is None else round(age_h,3),'schema_required':meta.get('schema_required'),'problems':problems}
    print(json.dumps(report,separators=(',',':')))
    return 0 if not problems else 2

if __name__=='__main__': raise SystemExit(main())
