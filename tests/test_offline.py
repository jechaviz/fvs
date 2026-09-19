#!/usr/bin/env python3
from __future__ import annotations
import hashlib,hmac,importlib.util,json,os,re,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'shim-python'))
import core,payments,inventory_import,app,enterprise

def check(cond,msg):
    if not cond: raise AssertionError(msg)

def main():
    check(core.version().startswith('6.0.0'),'core ABI version')
    check(payments.decimal_to_minor('123.45')==12345,'decimal exact')
    raw=b'{"id":"evt_test"}';ts=int(time.time());secret='whsec_test';sig=hmac.new(secret.encode(),str(ts).encode()+b'.'+raw,hashlib.sha256).hexdigest();payments.stripe_verify_signature(raw,f't={ts},v1={sig}',secret)
    mp_secret='mp-secret';rid='req-1';did='123';mts='1700000000';manifest=f'id:{did};request-id:{rid};ts:{mts};';msig=hmac.new(mp_secret.encode(),manifest.encode(),hashlib.sha256).hexdigest();payments.mp_verify_signature(f'ts={mts},v1={msig}',rid,did,mp_secret)

    # Payment resume validation must bind a remote object to the exact FVS attempt.
    attempt={"attempt_id":"11111111-1111-4111-8111-111111111111","cart_version":7,"amount_minor":12345,"currency":"MXN","email":"buyer@example.com","idempotency_key":"idem-test","provider_checkout_id":"pi_exact"}
    old_request=payments._request
    os.environ["STRIPE_SECRET_KEY"]="sk_test_offline"
    os.environ["MERCADOPAGO_ACCESS_TOKEN"]="mp_test_offline"
    try:
        payments._request=lambda *a,**k:{"object":"payment_intent","id":"pi_exact","amount":12345,"currency":"mxn","status":"requires_payment_method","metadata":{"attempt_id":attempt["attempt_id"],"cart_version":"7"},"client_secret":"cs_test"}
        check(payments.stripe_create_or_resume(attempt)["checkout_id"]=="pi_exact","stripe exact resume")
        payments._request=lambda *a,**k:{"object":"payment_intent","id":"pi_other","amount":12345,"currency":"mxn","status":"requires_payment_method","metadata":{"attempt_id":attempt["attempt_id"],"cart_version":"7"},"client_secret":"cs_test"}
        try: payments.stripe_create_or_resume(attempt); raise AssertionError("stripe mismatch accepted")
        except payments.ProviderError: pass
        mp_attempt={**attempt,"provider_checkout_id":"pref_exact"}
        pref={"id":"pref_exact","external_reference":attempt["attempt_id"],"items":[{"id":attempt["attempt_id"],"quantity":1,"currency_id":"MXN","unit_price":"123.45"}],"metadata":{"attempt_id":attempt["attempt_id"],"cart_version":"7"},"init_point":"https://www.mercadopago.com.mx/checkout/v1/redirect"}
        payments._request=lambda *a,**k:pref
        check(payments.mp_create_or_resume(mp_attempt,"https://example.test/hook","https://example.test/return")["checkout_id"]=="pref_exact","mp exact resume")
        bad=dict(pref);bad["metadata"]={}
        payments._request=lambda *a,**k:bad
        try: payments.mp_create_or_resume(mp_attempt,"https://example.test/hook","https://example.test/return"); raise AssertionError("mp metadata mismatch accepted")
        except payments.ProviderError: pass
    finally:
        payments._request=old_request

    for fn,args in [(payments.stripe_verify_signature,(b"{}","t=1,v1=x","")),(payments.mp_verify_signature,("ts=1,v1=x","r","d",""))]:
        try: fn(*args); raise AssertionError("empty webhook secret accepted")
        except payments.ProviderError: pass

    row=inventory_import.normalize({'resort':'R','unit_code':'U1','unit_name':'Suite','check_in':'2026-12-01','check_out':'2026-12-08','price':'1234.56','currency':'mxn','latitude':'21.16','longitude':'-86.85','property_type':'Villa','amenities':'pool;wifi','rating':'4.8'})
    check(row['price_minor']==123456 and row['currency']=='MXN' and row['booking_mode']=='fixed' and 1<=row['week_number']<=53,'import normalization')
    check(row['property_type']=='Villa' and row['amenities']==['pool','wifi'] and row['latitude']=='21.16' and row['rating']==4.8,'discovery metadata normalization')
    py=(ROOT/'shim-python/app.py').read_text();ph=(ROOT/'shim-php/index.php').read_text()
    routes=['/api/v1/health/live','/api/v1/health/ready','/api/v1/inventory','/api/v1/search/suggest','/api/v1/cart','/api/v1/admin/import','/api/v1/admin/ops','/api/v1/admin/metrics','/api/v1/admin/manual-review','/api/v1/webhooks/','/api/v1/support','/api/v1/agent/','/api/v1/marketing/event','/api/v1/admin/marketing/campaigns','/api/v1/admin/seo/rebuild','/robots.txt','/sitemap.xml','/stay/']
    for route in routes: check(route in py and route in ph,f'route parity {route}')
    for provider in ['stripe','mercadopago']: check(provider in py and provider in ph,f'provider parity {provider}')
    front='\n'.join(p.read_text(errors='ignore') for p in (ROOT/'frontend').rglob('*') if p.is_file())
    lowered=front.lower()
    for event in ('search','cart','checkout','booking'):
        check(f"this.track('{event}'" in front,f'funnel telemetry contract missing: {event}')
    check(not (ROOT/'frontend/package.json').exists(),'frontend has no Node package manifest')
    check(not re.search(r"(?:from\s+['\"](?:react|next(?:/|['\"])))|(?:src=['\"][^'\"]*(?:react|next)[^'\"]*)", lowered),'frontend has no React/Next framework dependency')
    check('fvs_cart_secret' not in front,'secret never referenced by frontend')
    check('innerHTML' not in front,'frontend avoids raw HTML sinks')
    serve=(ROOT/'shim-python/serve.py').read_text();check('BoundedSemaphore' in serve and 'settimeout' in serve,'python shim applies bounded backpressure and request timeout')
    pyworker=(ROOT/'workers/worker.py').read_text();phpworker=(ROOT/'workers/worker.php').read_text();check('FVS_EMAIL_CC' in pyworker and 'FVS_EMAIL_CC' in phpworker,'both workers support concierge/reservations CC')
    check('role="dialog"' in front and 'aria-modal="true"' in front,'booking drawer exposes dialog semantics')
    check('idempotency_key' not in front,'frontend never consumes provider idempotency key')
    app_py=(ROOT/'shim-python/app.py').read_text();app_php=(ROOT/'shim-php/index.php').read_text()
    check('_public_attempt' in app_py and 'publicAttempt' in app_php,'HTTP shims redact internal attempt fields')
    capture={}
    app._text(lambda status,headers:capture.update(status=status,headers=headers),200,'ok',cache='public,max-age=60')
    cache_headers=[v for k,v in capture['headers'] if k.lower()=='cache-control']
    check(cache_headers==['public,max-age=60'],'python text response emits exactly one requested cache policy')
    check('seo_redirect_get(path)' in app_py,'python SEO route honors redirect map')
    check('public,max-age=300,stale-while-revalidate=3600' in app_py,'python sitemap is cacheable')
    def bad_origin_status(path):
        from io import BytesIO
        cap={}
        env={'PATH_INFO':path,'REQUEST_METHOD':'POST','CONTENT_LENGTH':'2','wsgi.input':BytesIO(b'{}'),'HTTP_ORIGIN':'https://evil.example'}
        b''.join(app.application(env,lambda status,headers:cap.update(status=status,headers=headers)))
        return int(cap['status'].split()[0])
    for route in ['/api/v1/support','/api/v1/support/11111111-1111-4111-8111-111111111111/messages','/api/v1/marketing/event']:
        check(bad_origin_status(route)==403,f'origin gate enforced before browser mutation: {route}')
    support_ai_src=(ROOT/'workers/support_ai.py').read_text()
    check('FVS-support/4.0' not in support_ai_src and 'FVS-support/6.0.0' in support_ai_src,'support AI identifies current release')
    check('fvs_webhook_claim_v2' in (ROOT/'shim-python/core.py').read_text() and 'fvs_webhook_claim_v2' in (ROOT/'shim-php/fvs_ffi.h').read_text(),'webhook fencing v2 enabled')
    migrations='\n'.join(p.read_text() for p in sorted((ROOT/'migrations').glob('*.sql')))
    check('ALTER TABLE webhook_events' in migrations and 'lease_token CHAR(32)' in migrations,'webhook fencing column is migrated')
    check('resort_snapshot' in migrations and 'check_in_snapshot' in migrations,'cart contractual snapshot is migrated')
    check('support_threads' in migrations and 'support_messages' in migrations and 'support_events' in migrations,'hybrid support schema is migrated')
    check('marketing_campaigns' in migrations and 'marketing_jobs' in migrations and 'marketing_attribution_events' in migrations,'marketing automation schema is migrated')
    check('seo_pages' in migrations and 'seo_redirects' in migrations,'programmatic SEO schema is migrated')
    check('marketing_budget_events' in migrations and 'frequency_cap_7d' in migrations and 'stop_loss_minor' in migrations,'marketing guardrails and budget audit are migrated')
    check('commerce_events' in migrations and 'uq_commerce_event_key' in migrations and 'ix_commerce_event_currency' in migrations,'durable commerce telemetry schema is migrated')
    check('payment_recovery_cases' in migrations and 'ix_payment_recovery_state' in migrations,'payment recovery state is migrated')
    cmake=(ROOT/'core/CMakeLists.txt').read_text()
    check('src/internal.c' in cmake and 'src/telemetry.c' in cmake and 'src/payment_recovery.c' in cmake,'commerce telemetry/recovery are compiled as separate core modules')
    core_src=(ROOT/'core/src/core.c').read_text()
    check('fvs_inventory_upsert_v2' in core_src and 'booking_mode' in core_src and 'week_number_snapshot' in core_src and 'float_group_snapshot' in core_src,'week metadata flows through core')
    check('ci.price_minor=s.price_minor' in core_src and 'ci.hold_expires_at<=UTC_TIMESTAMP()' in core_src and "s.hold_expires_at<=UTC_TIMESTAMP()" in core_src,'expired hold repricing watches both hold clocks')
    check('!existing_pid[0]' in core_src and 'strcmp(existing_pid,provider_payment_id)!=0' in core_src,'payment confirmation requires a pre-bound provider payment id')
    check('quote_refreshed' in core_src and 'needs_requote' in core_src,'expired holds require explicit refreshed-quote confirmation')
    check("a.status==='quote_refreshed'" in front and 'this.checkout.terms=false' in front,'frontend forces review and renewed terms after requote')
    check('strcmp(existing_checkout,provider_payment_id)!=0' in core_src,'Stripe confirmation is bound to stored PaymentIntent')
    check('INSERT INTO cart_items' in core_src and "SELECT '%s',s.id" in core_src,'cart item snapshot insert is atomic')
    check('cart item limit exceeded' in core_src and 'cart total overflow' in core_src,'cart size/total guards enforced before checkout')
    migrator=(ROOT/'core/tools/migrate.c').read_text()
    check('FVS_DB_SSL_CA' in migrator and 'MYSQL_OPT_SSL_VERIFY_SERVER_CERT' in migrator and 'MYSQL_OPT_CONNECT_TIMEOUT' in migrator,'migrator mirrors runtime DB TLS/timeout controls')
    csp=(ROOT/'deploy/nginx/python.conf').read_text()+(ROOT/'deploy/nginx/php.conf').read_text()
    check('https://*.js.stripe.com' in csp and 'https://*.link.com' in csp,'Stripe Payment Element CSP coverage')
    check('https://unpkg.com' in csp and "worker-src 'self' blob:" in csp,'MapLibre CDN and worker CSP coverage')
    map_loader=(ROOT/'frontend/js/maplibre-loader.js').read_text()
    check("VERSION = '6.10.0'" in map_loader and 'maplibre-gl@${VERSION}' in map_loader and 'FVS_LOAD_MAPLIBRE' in map_loader and 'Buscar en esta zona' in front and 'TransitionGroup' in front,'lazy map/list discovery UI is wired')
    check('prefers-reduced-motion' in front and 'content-visibility:auto' in front,'dense UI keeps motion accessibility and rendering optimization')
    check('fvs_inventory_search_v2' in core_src and 'MATCH(search_text)' in core_src and 'fvs_inventory_suggest' in core_src,'full-text faceted discovery core is wired')
    check('FULLTEXT INDEX ft_inventory_search' in migrations and 'latitude DECIMAL' in migrations and 'amenities_json JSON' in migrations,'geo/full-text discovery migration present')

    check('commerce_promotions' in migrations and 'commerce_addons' in migrations and 'commerce_membership_plans' in migrations and 'commerce_loyalty_ledger' in migrations,'commerce monetization schema is migrated')
    check('social_sales_links' in migrations and 'social_catalog_items' in migrations and 'social_leads' in migrations,'social commerce schema is migrated')
    check('channel_scope JSON' in migrations and 'JSON_CONTAINS(p.channel_scope' in core_src,'channel-scoped promotions are enforced by the pricing/apply-code core')
    check('promotion usage exhausted' in core_src and 'referral usage exhausted' in core_src,'promo/referral usage limits are atomically enforced at booking confirmation')
    check("'send_conversion'" in core_src and 'social_sales_links SET bookings=bookings+1' in core_src and 'social_sales_link_revenue' in core_src,'social-origin bookings feed conversion automation and multi-currency link revenue counters')
    check('commerce_loyalty_ledger' in core_src and "'referral'" in core_src and 'points_multiplier_bps' in core_src,'booking loyalty and advocate referral rewards are ledgered idempotently')
    check('/api/v1/social/catalog/' in app_py and '/api/v1/social/inbound' in app_py and '/api/v1/social/catalog/' in app_php and '/api/v1/social/inbound' in app_php,'both shims expose authenticated catalog and inbound social commerce routes')
    check('FVS_SOCIAL_COMMERCE_ENABLED' in (ROOT/'deploy/preflight.sh').read_text() and 'FVS_SOCIAL_FEED_KEY_SHA256' in (ROOT/'deploy/preflight.sh').read_text() and 'FVS_SOCIAL_INBOUND_SECRET' in (ROOT/'deploy/preflight.sh').read_text(),'production preflight protects social commerce feeds and inbound leads')
    check('compare' in front.lower() and 'quick' in front.lower() and 'commerce' in front.lower(),'commerce UX includes comparison/quick decision surfaces')

    check('FVS_ERR_RETRY = 11' in (ROOT/'core/include/fvs.h').read_text(),'transient DB status is part of ABI')
    check('1205U || err == 1213U' in core_src,'deadlock and lock wait timeout are classified retryable')
    check('MYSQL_OPT_READ_TIMEOUT' in core_src and 'MYSQL_OPT_WRITE_TIMEOUT' in core_src and 'innodb_lock_wait_timeout' in core_src,'runtime DB timeouts are bounded')
    check((ROOT/'migrations/001_current.sql').exists() and 'ops_worker_heartbeats' in migrations and 'ops_actions' in migrations,'current schema contains production ops tables')
    check('FVS_MIGRATE_DB_USER' in migrator and 'FVS_MIGRATE_DB_PASSWORD' in migrator,'DDL credentials can be separated from runtime DML credentials')
    check("GET_LOCK('fvs_schema_migrate'" in migrator and "RELEASE_LOCK('fvs_schema_migrate'" in migrator,'migrations are serialized with a MySQL advisory lock')
    preflight=(ROOT/'deploy/preflight.sh').read_text();check('FVS_ENV' in preflight and 'FVS_COOKIE_SECURE' in preflight and 'FVS_DB_SSL_CA' in preflight,'production preflight is fail-closed')
    check('FVS_ADMIN_KEY_SHA256' in preflight and 'do not store plaintext FVS_ADMIN_KEY' in preflight,'production admin auth stores only a SHA-256 digest')
    check('FVS_ADMIN_KEY_SHA256' in app_py and 'FVS_ADMIN_KEY_SHA256' in app_php,'both shims verify hashed admin credentials')
    ent_py=(ROOT/'shim-python/enterprise.py').read_text(); ent_php=(ROOT/'shim-php/Enterprise.php').read_text()
    check('FVS_AGENT_KEY_SHA256' in ent_py and 'FVS_AGENT_KEY_SHA256' in ent_php and 'enterprise.agent_ok' in app_py and 'agentOk()' in app_php,'both shims verify hashed support-agent credentials')
    old_agent_hash=os.environ.get('FVS_AGENT_KEY_SHA256')
    try:
        os.environ['FVS_ENV']='production'; os.environ['FVS_AGENT_KEY_SHA256']=hashlib.sha256(b'correct-agent-key').hexdigest()
        check(enterprise.agent_ok({'HTTP_X_AGENT_KEY':'correct-agent-key'}),'python agent hash accepts correct key')
        check(not enterprise.agent_ok({'HTTP_X_AGENT_KEY':'wrong-agent-key'}),'python agent hash rejects wrong key')
    finally:
        if old_agent_hash is None: os.environ.pop('FVS_AGENT_KEY_SHA256',None)
        else: os.environ['FVS_AGENT_KEY_SHA256']=old_agent_hash
    old_hash=os.environ.get('FVS_ADMIN_KEY_SHA256'); old_env=os.environ.get('FVS_ENV')
    try:
        os.environ['FVS_ENV']='production'; os.environ['FVS_ADMIN_KEY_SHA256']=hashlib.sha256(b'correct-admin-key').hexdigest()
        check(app._admin_ok({'HTTP_X_ADMIN_KEY':'correct-admin-key'}),'python admin hash accepts correct key')
        check(not app._admin_ok({'HTTP_X_ADMIN_KEY':'wrong-admin-key'}),'python admin hash rejects wrong key')
    finally:
        if old_hash is None: os.environ.pop('FVS_ADMIN_KEY_SHA256',None)
        else: os.environ['FVS_ADMIN_KEY_SHA256']=old_hash
        if old_env is None: os.environ.pop('FVS_ENV',None)
        else: os.environ['FVS_ENV']=old_env
    check('fvs_ops_degraded' in app_py and 'fvs_ops_degraded' in app_php,'both shims expose Prometheus-format ops metrics')
    check('manual_review_list' in (ROOT/'shim-python/core.py').read_text() and 'manualReviewList' in (ROOT/'shim-php/Core.php').read_text(),'backoffice can inspect manual-review without direct SQL')
    fpm=(ROOT/'deploy/php-fpm-fvs.conf').read_text();check('pm.max_children' in fpm and 'request_terminate_timeout' in fpm and 'clear_env = no' in fpm,'PHP-FPM production pool is configured')
    check('fvs_json' in csp and 'limit_req_status 429' in csp and 'fvs_webhook' in csp,'Nginx has structured access logs and endpoint-specific rate limiting')
    webdf=(ROOT/'deploy/Dockerfile.web').read_text();webep=(ROOT/'deploy/web-entrypoint.sh').read_text();check('html-src' not in webdf and '/tmp/fvs-config.js' in webep,'web image keeps application assets immutable and writes only runtime config to tmpfs')
    comp=(ROOT/'deploy/compose.production.python.yml').read_text()+(ROOT/'deploy/compose.production.php.yml').read_text();check(comp.count('read_only: true')>=8,'production containers are read-only')
    shim_core=(ROOT/'shim-python/core.py').read_text(); ffi_php=(ROOT/'shim-php/fvs_ffi.h').read_text()
    check('fvs_webhook_claim.argtypes' not in shim_core and 'fvs_webhook_claim(' not in ffi_php,'owner-only webhook ABI is not exposed to shims')
    check("['checkout','paid','manual_review'].includes(this.cart?.status)" in front,'checkout auto-resume after reload')
    check('@3.5.42' in front and '@66.10.2' in front and '@0.9.5' in front,'CDN versions pinned')
    check(not (ROOT/'frontend/package.json').exists(),'no Node package manifest')
    support_ai=(ROOT/'workers/support_ai.py').read_text();marketing_py=(ROOT/'workers/marketing.py').read_text();marketing_php=(ROOT/'workers/marketing.php').read_text()
    check('https://api.openai.com/v1/responses' in support_ai and "'store':False" in support_ai and 'gpt-5.6-luna' in support_ai,'AI support uses Responses API without server-side response storage')
    check('safety_identifier' in support_ai and 'prompt_cache_key' in support_ai and 'hashlib.sha256' in support_ai,'AI support uses pseudonymous safety/cache identifiers without customer PII')
    check('[HANDOFF]' in support_ai and 'cargo no reconocido' in support_ai and 'modificar reserva' in support_ai,'AI support escalates sensitive service actions')
    check("FVS_SUPPORT_AI_ENABLED','1'" in support_ai and 'Asistencia IA deshabilitada' in support_ai,'runtime AI kill switch hands off to humans')
    check('PAN_RE' in support_ai and 'CVV_RE' in support_ai and '_redact_sensitive' in support_ai,'AI support redacts payment secrets before provider calls')
    check("'guardrails':job.get('guardrails')" in marketing_py and "'guardrails'=>$j['guardrails']" in marketing_php,'marketing connectors receive approved guardrails')
    check('marketing_metric_upsert' in marketing_py and 'marketingMetricUpsert' in marketing_php,'marketing workers ingest normalized channel metrics')
    check('commerce_event_record' in shim_core and 'commerceEventRecord' in (ROOT/'shim-php/Core.php').read_text(),'both shims emit authoritative commerce telemetry')
    check('commerce_funnel_dashboard' in shim_core and 'commerceFunnelDashboard' in (ROOT/'shim-php/Core.php').read_text(),'both shims expose authoritative funnel metrics')
    check('payment_recovery_mark' in shim_core and 'paymentRecoveryMark' in (ROOT/'shim-php/Core.php').read_text(),'both shims expose payment recovery state')
    check((ROOT/'shim-python/recovery.py').exists() and (ROOT/'shim-php/Recovery.php').exists(),'provider recovery is extracted from HTTP hotspots')
    check('def _provider_prepare' not in app_py and 'function providerPrepare(' not in app_php,'provider preparation no longer lives in HTTP entrypoints')
    check("event_type=\"search\"" in shim_core and "event_type=\"cart_add\"" in shim_core and "event_type=\"checkout_start\"" in shim_core and "event_type=\"booking\"" in shim_core,'python commerce funnel instrumentation is wired')
    check('fvs_marketing_campaign_configure' in core_src and 'fvs_marketing_campaign_pause' in core_src,'core owns campaign financial guardrails and pause')
    check('c.budget_minor=0' in core_src and 'c.daily_budget_minor=0' in core_src and 'c.max_daily_spend_minor=0' in core_src,'job claiming enforces lifetime and daily budget ceilings')
    check('GREATEST(COALESCE((SELECT SUM(ms.spend_minor)' in core_src and 'SUM(mr.revenue_minor)' in core_src,'stop-loss is enforced on net loss rather than gross spend')
    check("j.action<>'update_budget' OR c.target_roas_bps=0" in core_src,'automated budget increases are blocked below target ROAS')
    check('frequency_cap_7d' in core_src and "'guardrails':job.get('guardrails')" in marketing_py,'frequency cap is carried to the channel adapter for audience-level enforcement')
    check("event_type='order.confirmed' THEN 0" in core_src and "event_type='support.ai_requested' THEN 10" in core_src,'booking fulfillment outranks AI support work in shared outbox')
    check("CONCAT('vacation-rental-',LEFT(s.id,8))" in core_src and "CONCAT('/stay/vacation-rental-',LEFT(s.id,8))" in core_src,'SEO inventory slugs are deterministic ASCII from UUID hex')
    fake_page={'page_type':'property','locale':'es-MX','title':'Suite <Test>','meta_description':'Desc </script><script>alert(1)</script>','h1':'Suite & Spa','canonical_path':'/stay/vacation-rental-deadbeef','indexable':True,'faq':[{'question':'¿Pago?','answer':'Seguro'}],'property':{'slot_id':'deadbeef-dead-4bee-8eef-deadbeef0001','resort':'Resort','unit_name':'Suite','city':'Querétaro','country':'MX','check_in':'2026-12-01','check_out':'2026-12-08','price_minor':123456,'currency':'MXN','max_guests':4,'image_url':'https://cdn.example.test/p.jpg'}}
    rendered=enterprise.seo_html(fake_page)
    check('LodgingBusiness' in rendered and 'Accommodation' in rendered and 'VacationRental' not in rendered,'SEO emits generic valid lodging schema until VacationRental eligibility data is complete')
    check('&lt;Test&gt;' in rendered and '</script><script>alert(1)' not in rendered,'SEO renderer escapes HTML/script breakouts')
    check('rel="canonical"' in rendered and 'max-image-preview:large' in rendered,'SEO renderer emits canonical and index directives')
    support_spec=importlib.util.spec_from_file_location('fvs_support_ai',ROOT/'workers/support_ai.py');support_mod=importlib.util.module_from_spec(support_spec);support_spec.loader.exec_module(support_mod)
    redacted=support_mod._redact_sensitive('Tarjeta 4111 1111 1111 1111 y CVV: 123')
    check('4111' not in redacted and '123' not in redacted and 'DATOS_DE_PAGO_REDACTADOS' in redacted,'AI support payment-data redaction')
    old_enabled=os.environ.get('FVS_SUPPORT_AI_ENABLED');old_handoff=support_mod.core.support_handoff;handoffs=[]
    try:
        os.environ['FVS_SUPPORT_AI_ENABLED']='0';support_mod.core.support_handoff=lambda tid,reason: handoffs.append((tid,reason))
        check(support_mod.process_event({'event_type':'support.ai_requested','aggregate_id':'11111111-1111-4111-8111-111111111111','payload':{}}) is True and len(handoffs)==1,'runtime AI kill switch')
    finally:
        support_mod.core.support_handoff=old_handoff
        if old_enabled is None: os.environ.pop('FVS_SUPPORT_AI_ENABLED',None)
        else: os.environ['FVS_SUPPORT_AI_ENABLED']=old_enabled
    # Large carts must generate multipage vouchers instead of overflowing one sheet.
    workers_path=str(ROOT/'workers')
    if workers_path not in sys.path: sys.path.insert(0,workers_path)
    spec=importlib.util.spec_from_file_location('fvs_worker',ROOT/'workers/worker.py');worker=importlib.util.module_from_spec(spec);spec.loader.exec_module(worker)
    fake_item={'resort':'R','unit_name':'Suite','check_in':'2026-12-01','check_out':'2026-12-08','guests':2,'price_minor':10000,'currency':'MXN','week_number':49,'booking_mode':'fixed','float_group':None}
    fake_order={'order_id':'11111111-1111-4111-8111-111111111111','customer_email':'buyer@example.com','total_minor':640000,'currency':'MXN','items':[dict(fake_item) for _ in range(64)]}
    pdf=worker.make_pdf(fake_order);check(pdf.startswith(b'%PDF-1.4') and pdf.count(b'/Type /Page ' )>=4,'python voucher multipage')
    print('PASS offline contract/security tests')
if __name__=='__main__':main()
