#!/usr/bin/env python3
from __future__ import annotations
import hashlib,hmac,json,os,secrets,socket,sys,time,urllib.request,urllib.error,signal
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'shim-python'))
import core

OWNER_PREFIX=f"marketing:{socket.gethostname()}:{os.getpid()}";STOP=False

def _stop(signum,frame):
    global STOP;STOP=True
signal.signal(signal.SIGTERM,_stop);signal.signal(signal.SIGINT,_stop)

def _post(url:str,payload:dict,headers:dict)->dict:
    raw=json.dumps(payload,separators=(',',':'),ensure_ascii=False).encode()
    req=urllib.request.Request(url,data=raw,headers={'Content-Type':'application/json','User-Agent':'FVS-marketing/6.0',**headers},method='POST')
    try:
        with urllib.request.urlopen(req,timeout=float(os.getenv('FVS_MARKETING_HTTP_TIMEOUT','20'))) as r:
            data=r.read(1024*1024)
            if not data:return {'ok':True}
            return json.loads(data.decode())
    except urllib.error.HTTPError as e:
        detail=e.read(2048).decode('utf-8','replace')
        raise RuntimeError(f'provider_http_{e.code}:{detail[:300]}') from e

def _endpoint(channel:str)->tuple[str,dict]:
    key='FVS_MARKETING_'+channel.upper().replace('-','_')
    url=os.getenv(key+'_URL','').strip()
    if not url:raise RuntimeError(f'{key}_URL missing')
    token=os.getenv(key+'_TOKEN','').strip();headers={}
    if token:headers['Authorization']='Bearer '+token
    version=os.getenv(key+'_VERSION','').strip()
    if version:headers['X-API-Version']=version
    return url,headers

def dispatch(job:dict)->str:
    channel=str(job.get('channel') or '')
    provider_payload=job.get('payload') or {}
    if job.get('action')=='sync_catalog':
        provider_payload={**provider_payload,'catalog':core.social_catalog_feed(channel,500,0)}
    elif job.get('action')=='publish_collection' and provider_payload.get('collection_slug'):
        provider_payload={**provider_payload,'collection':core.commerce_collection_get(str(provider_payload['collection_slug']))}
    payload={
        'schema':'fvs.marketing.job.v2','job_id':job['job_id'],'campaign_id':job['campaign_id'],
        'creative_id':job.get('creative_id'),'channel':channel,'action':job['action'],
        'campaign_name':job.get('campaign_name'),'creative':{'headline':job.get('headline'),'body':job.get('body'),'cta':job.get('cta'),'landing_url':job.get('landing_url'),'image_url':job.get('image_url')},
        'provider_payload':provider_payload,'guardrails':job.get('guardrails') or {},
    }
    # A signed webhook connector is the safest universal adapter. Direct social APIs can
    # point FVS_MARKETING_<CHANNEL>_URL at an internal integration service or official endpoint.
    url,headers=_endpoint(channel)
    secret=os.getenv('FVS_MARKETING_SIGNING_SECRET','')
    if secret:
        raw=json.dumps(payload,separators=(',',':'),ensure_ascii=False).encode()
        headers['X-FVS-Signature']='sha256='+hmac.new(secret.encode(),raw,hashlib.sha256).hexdigest()
    result=_post(url,payload,headers)
    if job.get('action')=='sync_metrics' and isinstance(result.get('metrics'),dict):
        m=result['metrics']; metric_date=str(m.get('metric_date') or time.strftime('%Y-%m-%d',time.gmtime()))
        core.marketing_metric_upsert(job['campaign_id'],channel,metric_date,int(m.get('impressions') or 0),int(m.get('clicks') or 0),int(m.get('spend_minor') or 0),int(m.get('leads') or 0),int(m.get('bookings') or 0),int(m.get('revenue_minor') or 0),str(m.get('currency') or 'MXN'))
        core.experiment_snapshot(job['campaign_id'],30)
    return str(result.get('id') or result.get('provider_ref') or result.get('campaign_id') or '')[:191]

def once()->bool:
    owner=OWNER_PREFIX+':'+secrets.token_hex(8)
    job=core.marketing_job_claim(owner,int(os.getenv('FVS_MARKETING_LEASE_SECONDS','180')))
    if not job:return False
    try:
        provider_ref=dispatch(job)
        if job.get('action') in {'pause','update_budget'}:core.growth_decision_apply(job['job_id'])
        core.marketing_job_ack(job['job_id'],owner,job['lease_token'],provider_ref)
    except Exception as e:
        print(f"marketing error job={job.get('job_id')} type={type(e).__name__}",file=sys.stderr)
        try:core.marketing_job_nack(job['job_id'],owner,job['lease_token'],type(e).__name__)
        except Exception:pass
    return True

if __name__=='__main__':
    instance=OWNER_PREFIX;idle=max(.2,float(os.getenv('FVS_MARKETING_IDLE_SECONDS','2')));heartbeat=float(os.getenv('FVS_WORKER_HEARTBEAT_SECONDS','15'));next_hb=0.0;growth_every=max(60.0,float(os.getenv('FVS_GROWTH_LOOP_SECONDS','300')));next_growth=0.0
    while not STOP:
        now=time.monotonic()
        if now>=next_hb:
            try:
                core.worker_heartbeat('marketing',instance)
                core.abandonment_scan(int(os.getenv('FVS_ABANDONMENT_IDLE_SECONDS','1800')),int(os.getenv('FVS_ABANDONMENT_SCAN_LIMIT','50')))
            except Exception as e:print(f"marketing heartbeat/scanner error {type(e).__name__}",file=sys.stderr)
            next_hb=now+heartbeat
        if now>=next_growth:
            try:core.growth_loop_run(int(os.getenv('FVS_GROWTH_LOOP_DAYS','7')),int(os.getenv('FVS_GROWTH_LOOP_LIMIT','50')))
            except Exception as e:print(f"marketing growth-loop error {type(e).__name__}",file=sys.stderr)
            next_growth=now+growth_every
        if not once():time.sleep(idle)
    try:core.worker_goodbye('marketing',instance)
    except Exception:pass
