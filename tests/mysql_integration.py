#!/usr/bin/env python3
"""Run against MySQL 8 after migrations. No payment-provider network is required."""
from __future__ import annotations
import os,sys,shutil,subprocess,threading,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'shim-python'));sys.path.insert(0,str(ROOT/'scripts'))
import core,search_eval

def check(cond,msg):
    if not cond: raise AssertionError(msg)

TEST_CONTAINER=os.getenv("FVS_TEST_MYSQL_CONTAINER","")
def sql_exec(sql:str):
    if TEST_CONTAINER:
        cmd=["docker","exec",TEST_CONTAINER,"mysql","-uroot","-proot","fvs","--batch","--skip-column-names","-e",sql]
        subprocess.run(cmd,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        return
    cli=shutil.which("mysql") or shutil.which("mariadb")
    if not cli:
        raise RuntimeError("mysql/mariadb CLI or FVS_TEST_MYSQL_CONTAINER is required for deterministic integration mutations")
    env=os.environ.copy();env["MYSQL_PWD"]=os.getenv("FVS_DB_PASSWORD","")
    cmd=[cli,"-h",os.getenv("FVS_DB_HOST","127.0.0.1"),"-P",os.getenv("FVS_DB_PORT","3306"),"-u",os.getenv("FVS_DB_USER","fvs"),os.getenv("FVS_DB_NAME","fvs"),"--batch","--skip-column-names","-e",sql]
    subprocess.run(cmd,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,env=env)


def sql_value(sql:str)->str:
    if TEST_CONTAINER:
        cmd=["docker","exec",TEST_CONTAINER,"mysql","-uroot","-proot","fvs","--batch","--skip-column-names","-e",sql]
        return subprocess.check_output(cmd,stderr=subprocess.DEVNULL,text=True).strip()
    cli=shutil.which("mysql") or shutil.which("mariadb")
    if not cli: raise RuntimeError("mysql/mariadb CLI or FVS_TEST_MYSQL_CONTAINER is required")
    env=os.environ.copy();env["MYSQL_PWD"]=os.getenv("FVS_DB_PASSWORD","")
    cmd=[cli,"-h",os.getenv("FVS_DB_HOST","127.0.0.1"),"-P",os.getenv("FVS_DB_PORT","3306"),"-u",os.getenv("FVS_DB_USER","fvs"),os.getenv("FVS_DB_NAME","fvs"),"--batch","--skip-column-names","-e",sql]
    return subprocess.check_output(cmd,stderr=subprocess.DEVNULL,text=True,env=env).strip()

def main():
    os.environ.setdefault("FVS_DB_LOCK_WAIT_TIMEOUT","1")
    core.close_thread_ctx()
    tag=uuid.uuid4().hex[:10];unit='RACE-'+tag
    core.inventory_upsert({'source_ref':'itest:'+tag,'resort':'FVS Integration Resort','unit_code':unit,'unit_name':'Concurrency Suite','city':'Queretaro','country':'MX','check_in':'2027-01-10','check_out':'2027-01-17','max_guests':4,'price_minor':2500000,'currency':'MXN','active':True})
    items=core.inventory_search('2027-01-01','2027-01-31',2)['items'];slot=next(x['id'] for x in items if x['unit_code']==unit)

    # Discovery search: full-text-like ranking, facets, autosuggest, amenities and map bounds share one contract.
    disc=uuid.uuid4().hex[:8]
    rows=[
      {'source_ref':'disc:cancun:'+disc,'resort':'Coral Bay Discovery','unit_code':'DISC-CUN-'+disc,'unit_name':'Ocean Villa','city':'Cancun','country':'Mexico','check_in':'2027-08-10','check_out':'2027-08-17','max_guests':6,'price_minor':2200000,'currency':'MXN','active':True,'latitude':21.1619,'longitude':-86.8515,'property_type':'Villa','bedrooms':3,'bathrooms':2.5,'rating':4.8,'amenities':['pool','beach','wifi']},
      {'source_ref':'disc:tulum:'+disc,'resort':'Jungle House Discovery','unit_code':'DISC-TUL-'+disc,'unit_name':'Garden Residence','city':'Tulum','country':'Mexico','check_in':'2027-08-10','check_out':'2027-08-17','max_guests':4,'price_minor':1800000,'currency':'MXN','active':True,'latitude':20.2114,'longitude':-87.4654,'property_type':'Residence','bedrooms':2,'bathrooms':2,'rating':4.5,'amenities':['pool','wifi']},
      {'source_ref':'disc:ibiza:'+disc,'resort':'Azure Ibiza Discovery','unit_code':'DISC-IBZ-'+disc,'unit_name':'Sea Suite','city':'Ibiza','country':'Spain','check_in':'2027-08-10','check_out':'2027-08-17','max_guests':4,'price_minor':320000,'currency':'EUR','active':True,'latitude':38.9067,'longitude':1.4206,'property_type':'Suite','bedrooms':1,'bathrooms':1,'rating':4.9,'amenities':['beach','wifi']},
    ]
    for r in rows: core.inventory_upsert(r)
    discovery=core.inventory_search_v2({'q':'Coral','check_in':'2027-08-01','check_out':'2027-08-31','guests':2,'country':'Mexico','amenities':'pool','sort':'relevance','limit':50})
    check(any(x['unit_code'].startswith('DISC-CUN-') for x in discovery['items']),discovery)
    check(any(x['value']=='Mexico' for x in discovery['facets']['countries']),'country facet missing')
    check(discovery['items'][0]['property_type'] in {'Villa','Residence'},'discovery metadata missing')
    sug=core.inventory_suggest('Canc',10)['suggestions'];check(any(x['type']=='city' and x['label']=='Cancun' for x in sug),sug)
    bounded=core.inventory_search_v2({'check_in':'2027-08-01','check_out':'2027-08-31','guests':2,'bbox':[21.0,-87.0,21.3,-86.7],'sort':'relevance','limit':50})
    codes={x['unit_code'] for x in bounded['items']};check(any(x.startswith('DISC-CUN-') for x in codes) and not any(x.startswith('DISC-TUL-') for x in codes),'bbox filtering mismatch')
    refined=core.inventory_search_v2({'check_in':'2027-08-01','check_out':'2027-08-31','guests':2,'country':'Mexico','min_bedrooms':3,'min_bathrooms':2.5,'min_rating_x100':470,'sort':'rating','limit':50})
    refined_codes={x['unit_code'] for x in refined['items']};check(any(x.startswith('DISC-CUN-') for x in refined_codes) and not any(x.startswith('DISC-TUL-') for x in refined_codes),'bed/bath/rating filtering mismatch')

    # Two carts race for the exact same inventory row. Exactly one can own the hold.
    carts=[core.cart_create() for _ in range(2)];results=[];lock=threading.Lock();bar=threading.Barrier(2)
    def take(pair):
        cid,sec=pair
        try:bar.wait();r=core.cart_add(cid,sec,slot,2,900);v=('ok',cid,sec,r)
        except core.CoreError as e:v=('err',e.code,cid,sec)
        with lock:results.append(v)
    th=[threading.Thread(target=take,args=(p,)) for p in carts]
    [t.start() for t in th];[t.join() for t in th]
    oks=[r for r in results if r[0]=='ok'];errs=[r for r in results if r[0]=='err']
    check(len(oks)==1 and len(errs)==1 and errs[0][1]==4,results)

    # Checkout is resumable and preserves one immutable attempt for the cart version/provider.
    # Live holds preserve the quoted price even if an administrator updates inventory meanwhile.
    live_tag=uuid.uuid4().hex[:10];live_unit='LIVEPRICE-'+live_tag
    live_row={'source_ref':'itest:'+live_tag,'resort':'FVS Price Resort','unit_code':live_unit,'unit_name':'Live Quote Suite','city':'Queretaro','country':'MX','check_in':'2027-04-10','check_out':'2027-04-17','max_guests':4,'price_minor':1300000,'currency':'MXN','active':True}
    core.inventory_upsert(live_row)
    live_slot=next(x['id'] for x in core.inventory_search('2027-04-01','2027-04-30',2)['items'] if x['unit_code']==live_unit)
    live_cid,live_sec=core.cart_create();core.cart_add(live_cid,live_sec,live_slot,2,900)
    core.inventory_upsert({**live_row,'price_minor':1400000})
    live_attempt=core.checkout_begin(live_cid,live_sec,'stripe','liveprice@example.com','itest-v1',1200)
    check(live_attempt['amount_minor']==1300000,'active hold did not preserve quoted price')

    # Once either hold clock expires, checkout reprices from the current inventory contract.
    exp_tag=uuid.uuid4().hex[:10];exp_unit='REPRICE-'+exp_tag
    exp_row={'source_ref':'itest:'+exp_tag,'resort':'FVS Reprice Resort','unit_code':exp_unit,'unit_name':'Reprice Suite','city':'Queretaro','country':'MX','check_in':'2027-05-10','check_out':'2027-05-17','max_guests':4,'price_minor':1500000,'currency':'MXN','active':True}
    core.inventory_upsert(exp_row)
    exp_slot=next(x['id'] for x in core.inventory_search('2027-05-01','2027-05-31',2)['items'] if x['unit_code']==exp_unit)
    exp_cid,exp_sec=core.cart_create();core.cart_add(exp_cid,exp_sec,exp_slot,2,900)
    core.inventory_upsert({**exp_row,'price_minor':1750000})
    sql_exec(f"UPDATE inventory_slots SET hold_expires_at=TIMESTAMPADD(SECOND,-1,UTC_TIMESTAMP()) WHERE id='{exp_slot}'; UPDATE cart_items SET hold_expires_at=TIMESTAMPADD(SECOND,-1,UTC_TIMESTAMP()) WHERE cart_id='{exp_cid}' AND slot_id='{exp_slot}';")
    refreshed=core.checkout_begin(exp_cid,exp_sec,'stripe','reprice@example.com','itest-v1',1200)
    check(refreshed['status']=='quote_refreshed' and refreshed['amount_minor']==1750000,'expired hold did not require review of refreshed quote')
    check(core.cart_get(exp_cid,exp_sec)['total_minor']==1750000,'refreshed cart did not expose current total')
    exp_attempt=core.checkout_begin(exp_cid,exp_sec,'stripe','reprice@example.com','itest-v1',1200)
    check(exp_attempt['amount_minor']==1750000 and exp_attempt['status']=='creating','reviewed refreshed quote did not proceed to checkout')

    _,cid,sec,_=oks[0];a=core.checkout_begin(cid,sec,'stripe','itest@example.com','itest-v1',1200)
    core.checkout_attach(a['attempt_id'],'pi_'+tag,'pi_'+tag,'pi_'+tag+'_secret_test',None)
    resumed=core.checkout_begin(cid,sec,'stripe','itest@example.com','itest-v1',1200)
    check(resumed['attempt_id']==a['attempt_id'],'checkout did not resume same attempt')
    # Mutate source inventory after checkout: the eventual order must preserve what the guest accepted.
    core.inventory_upsert({'source_ref':'itest:'+tag,'resort':'MUTATED AFTER CHECKOUT','unit_code':unit,'unit_name':'Changed Suite','city':'Changed','country':'MX','check_in':'2027-01-11','check_out':'2027-01-18','max_guests':4,'price_minor':9999999,'currency':'MXN','active':True})
    try:
        core.checkout_attach(a['attempt_id'],'pi_DIFFERENT',None,None,None)
        raise AssertionError('provider object mismatch was accepted')
    except core.CoreError as e:
        check(e.code==4,f'unexpected mismatch error {e.code}')

    # Simulate provider-confirmed payment; order creation and inventory booking are atomic.
    confirmed=core.payment_confirm(a['attempt_id'],'stripe','pi_'+tag,a['amount_minor'],a['currency'],a['cart_version'])
    check(confirmed['status']=='confirmed',confirmed)
    again=core.payment_confirm(a['attempt_id'],'stripe','pi_'+tag,a['amount_minor'],a['currency'],a['cart_version'])
    check(again==confirmed,'payment confirmation is not idempotent')

    # Unbound provider payments are never auto-confirmed. Use an isolated cart because rejection intentionally quarantines it.
    tag3=uuid.uuid4().hex[:10];unit3='UNBOUND-'+tag3
    row3={'source_ref':'itest:'+tag3,'resort':'FVS Binding Resort','unit_code':unit3,'unit_name':'Binding Suite','city':'Queretaro','country':'MX','check_in':'2027-03-10','check_out':'2027-03-17','max_guests':4,'price_minor':1700000,'currency':'MXN','active':True}
    core.inventory_upsert(row3)
    slot3=next(x['id'] for x in core.inventory_search('2027-03-01','2027-03-31',2)['items'] if x['unit_code']==unit3)
    cid3,sec3=core.cart_create();core.cart_add(cid3,sec3,slot3,2,900);a3=core.checkout_begin(cid3,sec3,'stripe','binding@example.com','itest-v1',1200)
    core.checkout_attach(a3['attempt_id'],'pi_'+tag3,None,'pi_'+tag3+'_secret_test',None)
    rejected=core.payment_confirm(a3['attempt_id'],'stripe','pi_'+tag3,a3['amount_minor'],a3['currency'],a3['cart_version'])
    check(rejected['status']=='manual_review','unbound provider payment was auto-confirmed')

    # A post-checkout administrative inventory change must not create a partial/invalid booking after money succeeds.
    tag2=uuid.uuid4().hex[:10];unit2='REVIEW-'+tag2
    row2={'source_ref':'itest:'+tag2,'resort':'FVS Review Resort','unit_code':unit2,'unit_name':'Review Suite','city':'Queretaro','country':'MX','check_in':'2027-02-10','check_out':'2027-02-17','max_guests':4,'price_minor':1800000,'currency':'MXN','active':True}
    core.inventory_upsert(row2)
    slot2=next(x['id'] for x in core.inventory_search('2027-02-01','2027-02-28',2)['items'] if x['unit_code']==unit2)
    cid2,sec2=core.cart_create();core.cart_add(cid2,sec2,slot2,2,900);a2=core.checkout_begin(cid2,sec2,'stripe','review@example.com','itest-v1',1200);core.checkout_attach(a2['attempt_id'],'pi_'+tag2,'pi_'+tag2,'pi_'+tag2+'_secret_test',None)
    core.inventory_upsert({**row2,'active':False})
    review=core.payment_confirm(a2['attempt_id'],'stripe','pi_'+tag2,a2['amount_minor'],a2['currency'],a2['cart_version'])
    check(review['status']=='manual_review','inactive inventory after checkout was confirmed instead of reviewed')
    check(core.checkout_get(cid2,sec2)['status']=='manual_review','manual-review state not persisted on payment attempt')

    # Outbox + webhook fencing: stale owners/tokens cannot complete after lease expiry.
    owner1='itest-a:'+tag;evt=core.outbox_claim(owner1,5)
    check(evt.get('event_type')=='order.confirmed',evt);token1=evt['lease_token']
    wh_owner1='itest-wh-a:'+tag;wh_claim1,wh_token1=core.webhook_claim('stripe','evt_'+tag,wh_owner1,5)
    check(wh_claim1==1 and len(wh_token1)==32,'webhook first lease not acquired')
    wh_busy,_=core.webhook_claim('stripe','evt_'+tag,'itest-wh-busy:'+tag,5)
    check(wh_busy==2,'concurrent webhook claim was not fenced')
    order=core.order_get(confirmed['order_id']);check(len(order['items'])==1,'order snapshot missing item')
    oi=order['items'][0];check(oi['resort']=='FVS Integration Resort' and oi['check_in']=='2027-01-10' and oi['price_minor']==2500000,'order did not preserve checkout snapshot')
    sql_exec(f"UPDATE webhook_events SET lease_until=TIMESTAMPADD(SECOND,-1,UTC_TIMESTAMP()) WHERE provider='stripe' AND event_id='evt_{tag}'; UPDATE outbox_events SET lease_until=TIMESTAMPADD(SECOND,-1,UTC_TIMESTAMP()) WHERE id='{evt['event_id']}';")
    wh_owner2='itest-wh-b:'+tag;wh_claim2,wh_token2=core.webhook_claim('stripe','evt_'+tag,wh_owner2,120)
    check(wh_claim2==1 and wh_token2!=wh_token1,'expired webhook lease was not reclaimed with a new fence')
    try:
        core.webhook_complete('stripe','evt_'+tag,wh_owner1,wh_token1,True)
        raise AssertionError('stale webhook lease completed')
    except core.CoreError as e:
        check(e.code==4,f'unexpected stale webhook completion code {e.code}')
    core.webhook_complete('stripe','evt_'+tag,wh_owner2,wh_token2,True)
    wh_done,_=core.webhook_claim('stripe','evt_'+tag,'itest-wh-done:'+tag,120)
    check(wh_done==3,'processed webhook was claimable again')
    owner2='itest-b:'+tag;evt2=core.outbox_claim(owner2,120)
    check(evt2.get('event_id')==evt['event_id'],'expired processing event was not reclaimed')
    check(evt2.get('lease_token')!=token1,'reclaim did not rotate fencing token')
    check(evt2.get('attempts')==2,'claim attempts not incremented')
    try:
        core.outbox_ack(evt['event_id'],owner1,token1)
        raise AssertionError('stale outbox lease acknowledged')
    except core.CoreError as e:
        check(e.code==4,f'unexpected stale-ack code {e.code}')
    core.outbox_ack(evt2['event_id'],owner2,evt2['lease_token'])
    check(core.outbox_claim('itest-empty:'+tag,120)=={},'done outbox event was claimed again')

    # Lock wait timeout is surfaced as FVS_ERR_RETRY (11), never as a generic 500.
    lock_tag=uuid.uuid4().hex[:10];lock_unit='LOCK-'+lock_tag
    lock_row={'source_ref':'itest:'+lock_tag,'resort':'FVS Lock Resort','unit_code':lock_unit,'unit_name':'Lock Suite','city':'Queretaro','country':'MX','check_in':'2027-06-10','check_out':'2027-06-17','max_guests':4,'price_minor':1100000,'currency':'MXN','active':True}
    core.inventory_upsert(lock_row)
    lock_slot=next(x['id'] for x in core.inventory_search('2027-06-01','2027-06-30',2)['items'] if x['unit_code']==lock_unit)
    if TEST_CONTAINER:
        cmd=["docker","exec",TEST_CONTAINER,"mysql","-uroot","-proot","fvs","-e",f"START TRANSACTION; SELECT id FROM inventory_slots WHERE id='{lock_slot}' FOR UPDATE; DO SLEEP(3); ROLLBACK;"]
        holder=subprocess.Popen(cmd,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    else:
        cli=shutil.which("mysql") or shutil.which("mariadb")
        env=os.environ.copy();env["MYSQL_PWD"]=os.getenv("FVS_DB_PASSWORD","")
        cmd=[cli,"-h",os.getenv("FVS_DB_HOST","127.0.0.1"),"-P",os.getenv("FVS_DB_PORT","3306"),"-u",os.getenv("FVS_DB_USER","fvs"),os.getenv("FVS_DB_NAME","fvs"),"-e",f"START TRANSACTION; SELECT id FROM inventory_slots WHERE id='{lock_slot}' FOR UPDATE; DO SLEEP(3); ROLLBACK;"]
        holder=subprocess.Popen(cmd,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,env=env)
    import time as _time; _time.sleep(0.5)
    lock_cid,lock_sec=core.cart_create()
    try:
        core.cart_add(lock_cid,lock_sec,lock_slot,2,900);raise AssertionError('lock wait did not time out')
    except core.CoreError as e:
        check(e.code==11,f'lock wait not classified retryable: {e.code}')
    finally: holder.wait(timeout=10)

    # Production readiness/ops plane.
    schema=core.schema_status('001_current.sql');check(schema.get('ready') is True and schema.get('dirty_count')==0,schema)
    sql_exec("UPDATE schema_migrations SET dirty=1 WHERE version='001_current.sql'")
    dirty_state=core.schema_status('001_current.sql');check(dirty_state.get('ready') is False and dirty_state.get('dirty_count',0)>=1,dirty_state)
    sql_exec("UPDATE schema_migrations SET dirty=0 WHERE version='001_current.sql'")
    instance='itest-worker:'+tag;core.worker_heartbeat('outbox',instance)
    ops=core.ops_snapshot(90);check(ops.get('workers',0)>=1 and 'outbox_dead' in ops and 'workers_stale' in ops,ops)
    active_before=int(ops.get('workers',0));core.worker_goodbye('outbox',instance);ops_after=core.ops_snapshot(90)
    check(int(ops_after.get('workers',0))==active_before-1,'gracefully stopped worker still counted active')
    check(sql_value(f"SELECT COUNT(*) FROM ops_worker_heartbeats WHERE instance_id='{instance}' AND stopped_at IS NOT NULL")=='1','worker goodbye did not persist lifecycle stop')
    dead_id=str(uuid.uuid4());aggregate=confirmed['order_id']
    sql_exec(f"INSERT INTO outbox_events(id,event_type,aggregate_id,payload_json,status,attempts,next_attempt_at,created_at,updated_at) VALUES('{dead_id}','order.confirmed','{aggregate}',JSON_OBJECT('order_id','{aggregate}'),'dead',8,UTC_TIMESTAMP(),UTC_TIMESTAMP(),UTC_TIMESTAMP())")
    core.outbox_requeue_dead(dead_id,'itest-admin')
    check(sql_value(f"SELECT CONCAT(status,':',attempts) FROM outbox_events WHERE id='{dead_id}'")=='retry:0','dead-letter requeue failed')
    check(sql_value(f"SELECT COUNT(*) FROM ops_actions WHERE action_type='outbox.requeue' AND target_id='{dead_id}'")=='1','dead-letter requeue was not audited')
    # Enterprise support: customer -> AI -> human handoff retains one audited thread.
    support_id,support_secret=core.support_thread_create('support-itest@example.com','Enterprise support integration',priority='high')
    thread=core.support_customer_message(support_id,support_secret,'Necesito información general de mi reserva')
    check(thread['thread_id']==support_id and thread['messages'][-1]['sender_type']=='customer','support customer message missing')
    ai_thread=core.support_ai_message(support_id,'Con gusto te ayudo con información general.','itest-ai',12,18)
    check(ai_thread['messages'][-1]['sender_type']=='ai' and ai_thread['status']=='waiting_customer','AI support reply/state mismatch')
    core.support_customer_message(support_id,support_secret,'Ahora necesito cambiar la reserva')
    core.support_handoff(support_id,'integration sensitive action')
    agent_id='itest-agent-'+tag
    core.support_agent_heartbeat(agent_id,'Integration Agent','available',3)
    queued=core.support_queue_list(100)['threads']
    check(any(x['thread_id']==support_id and x['status']=='waiting_human' for x in queued),'handoff not visible in agent queue')
    core.support_agent_claim(support_id,agent_id)
    owned=core.support_agent_message(support_id,agent_id,'Tomé el caso; revisaremos el cambio con control humano.',False)
    check(owned['assigned_agent_id']==agent_id and owned['status']=='waiting_customer','agent ownership/reply mismatch')
    core.support_thread_resolve(support_id,agent_id)
    check(core.support_thread_context(support_id)['status']=='resolved','support thread did not resolve')

    # Growth automation: approval gates, financial guardrails, fencing, metrics and attribution.
    campaign=core.marketing_campaign_create('Integration Growth '+tag,'bookings','guarded',500000,50000,'MXN','itest-'+tag,'itest-admin')
    campaign_id=campaign['campaign_id']
    core.marketing_campaign_configure(campaign_id,max_daily_spend_minor=45000,frequency_cap_7d=5,target_roas_bps=25000,stop_loss_minor=180000,audience={'segment':'itest'},geo={'countries':['MX']},placements={'channels':['webhook']},optimization_rules={'pause_below_roas_bps':12000},experiment={'allocation':'50/50'},actor='itest-admin')
    creative=core.marketing_creative_add(campaign_id,'webhook','A','Reserva tu semana','Disponibilidad limitada de integración','Reservar','https://example.test/stay','',True,'itest-admin')
    creative_id=creative['creative_id']
    try:
        core.marketing_job_schedule(campaign_id,creative_id,'webhook','publish','2026-01-01 00:00:00',{'test':True},'itest-admin')
        raise AssertionError('unapproved campaign/creative scheduled a marketing job')
    except core.CoreError as e:
        check(e.code==4,f'unexpected marketing approval-gate code {e.code}')
    core.marketing_campaign_approve(campaign_id,'itest-approver')
    core.marketing_creative_approve(creative_id,'itest-approver')
    scheduled=core.marketing_job_schedule(campaign_id,creative_id,'webhook','publish','2026-01-01 00:00:00',{'test':True},'itest-admin')
    owner='itest-marketing:'+tag
    claimed=core.marketing_job_claim(owner,120)
    check(claimed.get('job_id')==scheduled['job_id'],'marketing job not claimed')
    guard=claimed.get('guardrails') or {}
    check(int(guard.get('max_daily_spend_minor') or 0)==45000 and int(guard.get('frequency_cap_7d') or 0)==5,'marketing guardrails not carried to connector')
    token=claimed['lease_token']
    try:
        core.marketing_job_ack(claimed['job_id'],owner,'0'*32,'bad-ref')
        raise AssertionError('stale marketing fencing token acknowledged')
    except core.CoreError as e:
        check(e.code==4,f'unexpected marketing fencing code {e.code}')
    core.marketing_job_ack(claimed['job_id'],owner,token,'provider-itest-'+tag)
    core.marketing_metric_upsert(campaign_id,'webhook','2026-09-17',10000,325,20000,21,4,120000,'MXN')
    visitor=str(uuid.uuid4());session=str(uuid.uuid4())
    core.marketing_attribution_record(campaign_id=campaign_id,channel='webhook',creative_id=creative_id,visitor_id=visitor,session_id=session,event_type='landing',utm_source='itest',utm_medium='paid-social',utm_campaign='itest-'+tag,utm_content='A',referrer='https://example.test/')
    dashboard=core.marketing_dashboard(730)
    check(int(dashboard.get('spend_minor') or 0)>=20000 and int(dashboard.get('revenue_minor') or 0)>=120000,'marketing dashboard did not aggregate normalized metrics')
    # Once normalized provider spend reaches the approved lifetime budget, publish jobs remain unclaimed.
    core.marketing_metric_upsert(campaign_id,'webhook','2026-09-17',120000,4000,500000,250,18,620000,'MXN')
    capped=core.marketing_job_schedule(campaign_id,creative_id,'webhook','publish','2026-01-01 00:00:00',{'test':'budget-cap'},'itest-admin')
    check(core.marketing_job_claim(owner+'-budget',120)=={},'lifetime campaign budget did not block execution')
    check(sql_value(f"SELECT status FROM marketing_jobs WHERE id='{capped['job_id']}'")=='pending','budget-capped job should remain pending for operator action')
    core.marketing_campaign_pause(campaign_id,'itest-admin')
    check(sql_value(f"SELECT status FROM marketing_campaigns WHERE id='{campaign_id}'")=='paused','campaign pause not persisted')

    # Commerce 6.0: channel-scoped promo, checkout reservation, add-ons, social conversion and loyalty.
    promo_code=('TT'+tag).upper()
    core.commerce_promotion_upsert({'code':promo_code,'name':'TikTok integration offer','discount_type':'percent_bps','discount_value':1000,'min_subtotal_minor':0,'max_discount_minor':0,'currency':'MXN','usage_limit':2,'active':True,'channel_scope':['tiktok']},'itest-commerce')
    scope_tag=uuid.uuid4().hex[:8];scope_unit='SCOPE-'+scope_tag
    scope_row={'source_ref':'commerce:scope:'+scope_tag,'resort':'Commerce Scope Resort','unit_code':scope_unit,'unit_name':'Scoped Promo Suite','city':'Cancun','country':'Mexico','check_in':'2027-09-10','check_out':'2027-09-17','max_guests':4,'price_minor':1600000,'currency':'MXN','active':True}
    core.inventory_upsert(scope_row);scope_slot=next(x['id'] for x in core.inventory_search('2027-09-01','2027-09-30',2)['items'] if x['unit_code']==scope_unit)
    organic_id,organic_sec=core.cart_create();core.cart_add(organic_id,organic_sec,scope_slot,2,900)
    try:
        core.cart_apply_code(organic_id,organic_sec,promo_code);raise AssertionError('channel-scoped promo applied without matching origin')
    except core.CoreError as e:
        check(e.code==3,f'unexpected scoped-promo rejection code {e.code}')

    social_tag=uuid.uuid4().hex[:8];social_unit='SOCIAL-'+social_tag
    social_row={'source_ref':'commerce:social:'+social_tag,'resort':'Social Commerce Resort','unit_code':social_unit,'unit_name':'Shoppable Villa','city':'Cancun','country':'Mexico','check_in':'2027-10-10','check_out':'2027-10-17','max_guests':6,'price_minor':1600000,'currency':'MXN','active':True}
    core.inventory_upsert(social_row);social_slot=next(x['id'] for x in core.inventory_search('2027-10-01','2027-10-31',2)['items'] if x['unit_code']==social_unit)
    social_link=core.social_sales_link_create({'channel':'tiktok','slot_id':social_slot,'campaign_id':campaign_id,'creative_id':creative_id,'promo_code':promo_code},'itest-social')
    resolved=core.social_sales_link_resolve(social_link['token']);check(resolved['channel']=='tiktok' and promo_code in resolved['path'],'social deep link did not preserve offer')
    social_cid,social_sec=core.cart_create();core.cart_add(social_cid,social_sec,social_slot,2,900);core.cart_set_origin(social_cid,social_sec,social_token=social_link['token'])
    social_cart=core.cart_addon_set(social_cid,social_sec,'CONCIERGE',1)
    check(social_cart['subtotal_minor']==1600000 and social_cart['addons_minor']==120000 and social_cart['discount_minor']==160000 and social_cart['total_minor']==1560000,social_cart)
    social_attempt=core.checkout_begin(social_cid,social_sec,'stripe','social-itest@example.com','itest-v1',1200);check(social_attempt['amount_minor']==1560000,social_attempt)
    social_pi='pi_social_'+tag;core.checkout_attach(social_attempt['attempt_id'],social_pi,social_pi,social_pi+'_secret',None)
    social_order=core.payment_confirm(social_attempt['attempt_id'],'stripe',social_pi,social_attempt['amount_minor'],social_attempt['currency'],social_attempt['cart_version']);check(social_order['status']=='confirmed',social_order)
    check(sql_value(f"SELECT bookings FROM social_sales_links WHERE token='{social_link['token']}'")=="1",'social sale booking counter mismatch')
    check(sql_value(f"SELECT CONCAT(r.bookings,':',r.revenue_minor) FROM social_sales_link_revenue r JOIN social_sales_links l ON l.id=r.link_id WHERE l.token='{social_link['token']}' AND r.currency='MXN'")==f"1:{social_attempt['amount_minor']}",'social sale MXN revenue counter mismatch')
    check(sql_value(f"SELECT COUNT(*) FROM marketing_jobs WHERE campaign_id='{campaign_id}' AND channel='tiktok' AND action='send_conversion' AND JSON_UNQUOTE(JSON_EXTRACT(payload_json,'$.order_id'))='{social_order['order_id']}'")=='1','social booking did not schedule conversion callback')
    buyer_points=int(sql_value("SELECT points_balance FROM commerce_loyalty_accounts WHERE email_hash=UNHEX(SHA2('social-itest@example.com',256))"));check(buyer_points==156,f'buyer loyalty mismatch: {buyer_points}')

    # Social attribution is multi-currency safe: revenue is bucketed by currency instead of mixed in the link row.
    eur_tag=uuid.uuid4().hex[:8];eur_unit='SOCIAL-EUR-'+eur_tag
    eur_row={'source_ref':'commerce:social-eur:'+eur_tag,'resort':'Social EUR Resort','unit_code':eur_unit,'unit_name':'Shoppable EUR Villa','city':'Madrid','country':'Spain','check_in':'2027-10-20','check_out':'2027-10-27','max_guests':4,'price_minor':90000,'currency':'EUR','active':True}
    core.inventory_upsert(eur_row);eur_slot=next(x['id'] for x in core.inventory_search('2027-10-18','2027-10-30',2)['items'] if x['unit_code']==eur_unit)
    eur_link=core.social_sales_link_create({'channel':'tiktok','slot_id':eur_slot,'campaign_id':campaign_id,'creative_id':creative_id},'itest-social-eur')
    eur_cid,eur_sec=core.cart_create();core.cart_add(eur_cid,eur_sec,eur_slot,2,900);core.cart_set_origin(eur_cid,eur_sec,social_token=eur_link['token'])
    eur_attempt=core.checkout_begin(eur_cid,eur_sec,'stripe','social-eur@example.com','itest-v1',1200);check(eur_attempt['currency']=='EUR' and eur_attempt['amount_minor']==90000,eur_attempt)
    eur_pi='pi_social_eur_'+tag;core.checkout_attach(eur_attempt['attempt_id'],eur_pi,eur_pi,eur_pi+'_secret',None)
    eur_order=core.payment_confirm(eur_attempt['attempt_id'],'stripe',eur_pi,eur_attempt['amount_minor'],eur_attempt['currency'],eur_attempt['cart_version']);check(eur_order['status']=='confirmed',eur_order)
    check(sql_value(f"SELECT bookings FROM social_sales_links WHERE token='{eur_link['token']}'")=="1",'EUR social sale booking counter mismatch')
    check(sql_value(f"SELECT CONCAT(r.bookings,':',r.revenue_minor) FROM social_sales_link_revenue r JOIN social_sales_links l ON l.id=r.link_id WHERE l.token='{eur_link['token']}' AND r.currency='EUR'")=="1:90000",'EUR social revenue bucket mismatch')
    check(sql_value(f"SELECT COUNT(*) FROM social_sales_link_revenue r JOIN social_sales_links l ON l.id=r.link_id WHERE l.token='{eur_link['token']}' AND r.currency<>'EUR'")=="0",'EUR social link mixed currencies')

    # A usage_limit=1 promotion is reserved by the first live checkout; a second cart is requoted without it.
    scarce=('ONE'+tag).upper();core.commerce_promotion_upsert({'code':scarce,'name':'Single-use integration offer','discount_type':'fixed_minor','discount_value':100000,'currency':'MXN','usage_limit':1,'active':True,'channel_scope':[]},'itest-commerce')
    scarce_slots=[]
    for ix in range(2):
        u=f'SCARCE-{ix}-{tag}';row={'source_ref':f'commerce:scarce:{ix}:{tag}','resort':'Scarce Promo Resort','unit_code':u,'unit_name':f'Scarce Suite {ix}','city':'Queretaro','country':'MX','check_in':f'2027-11-{10+ix*8:02d}','check_out':f'2027-11-{17+ix*8:02d}','max_guests':4,'price_minor':1000000,'currency':'MXN','active':True};core.inventory_upsert(row);scarce_slots.append(next(x['id'] for x in core.inventory_search('2027-11-01','2027-12-01',2)['items'] if x['unit_code']==u))
    sc=[]
    for slotx in scarce_slots:
        ci,cs=core.cart_create();core.cart_add(ci,cs,slotx,2,900);core.cart_apply_code(ci,cs,scarce);sc.append((ci,cs))
    first_s=core.checkout_begin(sc[0][0],sc[0][1],'stripe','scarce1@example.com','itest-v1',1200);check(first_s['amount_minor']==900000,first_s)
    second_s=core.checkout_begin(sc[1][0],sc[1][1],'stripe','scarce2@example.com','itest-v1',1200);check(second_s['status']=='quote_refreshed' and second_s['amount_minor']==1000000 and second_s.get('promo_code') is None,second_s)

    # Referral purchase rewards the advocate in an idempotent loyalty ledger.
    ref=('REF'+tag).upper();sql_exec(f"INSERT INTO commerce_referral_codes(id,code,owner_email_hash,invitee_discount_minor,advocate_points,currency,usage_limit,usage_count,active,created_at,updated_at) VALUES(UUID(),'{ref}',UNHEX(SHA2('advocate@example.com',256)),50000,25,'MXN',10,0,1,UTC_TIMESTAMP(),UTC_TIMESTAMP())")
    ref_tag=uuid.uuid4().hex[:8];ref_unit='REF-'+ref_tag;ref_row={'source_ref':'commerce:ref:'+ref_tag,'resort':'Referral Resort','unit_code':ref_unit,'unit_name':'Referral Suite','city':'Queretaro','country':'MX','check_in':'2027-12-10','check_out':'2027-12-17','max_guests':4,'price_minor':1000000,'currency':'MXN','active':True};core.inventory_upsert(ref_row);ref_slot=next(x['id'] for x in core.inventory_search('2027-12-01','2027-12-31',2)['items'] if x['unit_code']==ref_unit)
    ref_cid,ref_sec=core.cart_create();core.cart_add(ref_cid,ref_sec,ref_slot,2,900);ref_cart=core.cart_apply_code(ref_cid,ref_sec,ref);check(ref_cart['discount_minor']==50000,ref_cart)
    ref_attempt=core.checkout_begin(ref_cid,ref_sec,'stripe','invitee@example.com','itest-v1',1200);ref_pi='pi_ref_'+tag;core.checkout_attach(ref_attempt['attempt_id'],ref_pi,ref_pi,ref_pi+'_secret',None);ref_order=core.payment_confirm(ref_attempt['attempt_id'],'stripe',ref_pi,ref_attempt['amount_minor'],ref_attempt['currency'],ref_attempt['cart_version']);check(ref_order['status']=='confirmed',ref_order)
    advocate_points=int(sql_value("SELECT points_balance FROM commerce_loyalty_accounts WHERE email_hash=UNHEX(SHA2('advocate@example.com',256))"));check(advocate_points==25,f'advocate referral reward mismatch: {advocate_points}')
    check(sql_value(f"SELECT COUNT(*) FROM commerce_referral_events WHERE order_id='{ref_order['order_id']}' AND event_type='booking'")=='1','referral booking event missing')

    # Programmatic SEO: inventory pages are generated deterministically and custom destination pages render from the same core.
    rebuilt=core.seo_rebuild_inventory('itest-seo')
    check(rebuilt>=1,'SEO inventory rebuild processed no rows')
    seo_slug='vacation-rental-'+live_slot[:8]
    seo_page=core.seo_page_get(seo_slug,'es-MX')
    check(seo_page['page_type']=='property' and seo_page['property']['slot_id']==live_slot,'SEO property page mismatch')
    destination_slug='queretaro-itest-'+tag
    core.seo_page_upsert({'slug':destination_slug,'locale':'es-MX','page_type':'destination','title':'Estancias en Querétaro | FVS','meta_description':'Inventario de integración para pruebas SEO.','h1':'Estancias en Querétaro','body_text':'Página destino de integración.','faq':[{'question':'¿Cómo reservar?','answer':'Selecciona una estancia y agrégala al carrito.'}],'canonical_path':'/stay/'+destination_slug,'indexable':True},actor='itest-seo')
    dest=core.seo_page_get(destination_slug,'es-MX')
    check(dest['page_type']=='destination' and dest['canonical_path']=='/stay/'+destination_slug,'custom SEO destination page mismatch')
    listed=core.seo_pages_list(50000)['pages']
    check(any(x['slug']==destination_slug for x in listed),'destination page missing from sitemap source')

    # Authoritative funnel telemetry is idempotent and keeps money bucketed by currency.
    telemetry_key='itest:search:'+tag
    core.commerce_event_record(event_key=telemetry_key,event_type='search',source='server',query_text='no inventory integration query',result_count=0,metadata_json={'ranking_version':'search-v1'})
    core.commerce_event_record(event_key=telemetry_key,event_type='search',source='server',query_text='no inventory integration query',result_count=0,metadata_json={'ranking_version':'search-v1'})
    check(sql_value(f"SELECT COUNT(*) FROM commerce_events WHERE event_key='{telemetry_key}'")=='1','commerce telemetry idempotency failed')
    funnel=core.commerce_funnel_dashboard(1)
    check(funnel['searches']>=1 and funnel['zero_results']>=1,'funnel search metrics missing')
    check(funnel['bookings']>=1 and isinstance(funnel['revenue_by_currency'],list),'funnel booking/currency metrics missing')
    check(any(x['currency']=='MXN' for x in funnel['revenue_by_currency']),'MXN booking revenue bucket missing')
    check(any(x['currency']=='EUR' for x in funnel['revenue_by_currency']),'EUR booking revenue bucket missing')
    check(sql_value("SELECT COUNT(DISTINCT currency) FROM commerce_events WHERE event_type='booking' AND outcome='confirmed'")>='2','booking telemetry mixed/lost currency dimensions')

    # Payment recovery is explicit, idempotent by attempt, and never performs a payment itself.
    recovered=core.payment_recovery_get(a['attempt_id'])
    check(recovered['state']=='resolved' and recovered['retry_count']==0,recovered)
    reviewed=core.payment_recovery_get(a3['attempt_id'])
    check(reviewed['state']=='manual_review',reviewed)
    core.payment_recovery_mark(exp_attempt['attempt_id'],'provider_error','provider_prepare','ProviderError',30)
    core.payment_recovery_mark(exp_attempt['attempt_id'],'provider_error','provider_prepare','ProviderError',30)
    retry_case=core.payment_recovery_get(exp_attempt['attempt_id'])
    check(retry_case['state']=='provider_error' and retry_case['retry_count']==2 and retry_case['next_retry_at'],retry_case)
    recovery_dash=core.payment_recovery_dashboard(1)
    check(recovery_dash['provider_error']>=1 and recovery_dash['manual_review']>=1 and recovery_dash['resolved']>=1,recovery_dash)

    # Search science: fixed corpus, deterministic relevance metrics and persisted ranking evidence.
    corpus_path=ROOT/'tests/fixtures/search_eval_v1.json'
    corpus=search_eval.load_corpus(corpus_path)
    check(core.search_ranking_version()==corpus['ranking_version'],'ranking version drift')
    for row in corpus['inventory']:
        core.inventory_upsert(row)
    eval_rows=[]
    for case in corpus['queries']:
        result=core.inventory_search_v2({'q':case['query'],'check_in':'2027-08-01','check_out':'2027-08-31','guests':2,'sort':'relevance','limit':10})
        codes=[x['unit_code'] for x in result['items']]
        eval_rows.append(search_eval.evaluate_query(codes,case['relevance']))
    metrics=search_eval.aggregate(eval_rows)
    check(metrics['mrr']>=0.99,metrics)
    check(metrics['ndcg10']>=0.95,metrics)
    check(metrics['precision10']>=0.13,metrics)
    corpus_sha=search_eval.corpus_sha256(corpus_path)
    recorded=core.search_eval_record(corpus['ranking_version'],corpus_sha,metrics['mrr'],metrics['ndcg10'],metrics['precision10'],len(corpus['queries']))
    check(recorded['corpus_sha256']==corpus_sha and recorded['ranking_version']==corpus['ranking_version'],recorded)
    zero_query='zzzz-no-fvs-search-result-98765'
    zero=core.inventory_search_v2({'q':zero_query,'check_in':'2027-08-01','check_out':'2027-08-31','guests':2,'sort':'relevance','limit':10})
    check(zero['total']==0,zero)
    quality=core.search_quality_dashboard(1)
    check(quality['last_eval']['corpus_sha256']==corpus_sha,quality)
    check(any(x['version']==corpus['ranking_version'] for x in quality['ranking_versions']),quality)
    check(any(x['query']==zero_query for x in quality['top_zero_queries']),quality)

    print('PASS MySQL concurrency + booking + support + growth + commerce/social + SEO + telemetry + payment recovery + search science + production ops')
if __name__=='__main__':main()
