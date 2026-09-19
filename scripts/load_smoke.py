#!/usr/bin/env python3
from __future__ import annotations
import argparse, concurrent.futures, json, statistics, time, urllib.request, urllib.error

def one(url, timeout):
    start=time.perf_counter()
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            r.read(64); code=r.status
    except Exception:
        code=0
    return code,(time.perf_counter()-start)*1000

def pct(values,p):
    if not values:return 0.0
    xs=sorted(values);i=min(len(xs)-1,max(0,int(round((len(xs)-1)*p))))
    return xs[i]

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--base',default='http://127.0.0.1:8080');ap.add_argument('--seconds',type=int,default=20);ap.add_argument('--concurrency',type=int,default=20);ap.add_argument('--timeout',type=float,default=3.0);ap.add_argument('--max-error-rate',type=float,default=0.01);ap.add_argument('--max-p95-ms',type=float,default=500.0);a=ap.parse_args()
    url=a.base.rstrip('/')+'/api/v1/health/ready';end=time.monotonic()+a.seconds;lat=[];ok=0;err=0
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.concurrency) as ex:
        inflight={ex.submit(one,url,a.timeout) for _ in range(a.concurrency)}
        while inflight:
            done,inflight=concurrent.futures.wait(inflight,return_when=concurrent.futures.FIRST_COMPLETED)
            for fut in done:
                code,ms=fut.result();lat.append(ms);ok+=int(200<=code<300);err+=int(not 200<=code<300)
                if time.monotonic()<end:inflight.add(ex.submit(one,url,a.timeout))
    total=ok+err;error_rate=(err/total) if total else 1.0
    report={'requests':total,'ok':ok,'errors':err,'error_rate':round(error_rate,6),'p50_ms':round(pct(lat,.50),2),'p95_ms':round(pct(lat,.95),2),'p99_ms':round(pct(lat,.99),2),'rps':round(total/max(a.seconds,1),2)}
    print(json.dumps(report,separators=(',',':')))
    if error_rate>a.max_error_rate or report['p95_ms']>a.max_p95_ms:raise SystemExit(2)
if __name__=='__main__':main()
