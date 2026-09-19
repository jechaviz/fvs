from __future__ import annotations
import hashlib, hmac, json, logging, os, re, secrets, traceback
from http import HTTPStatus
from http.cookies import SimpleCookie
from urllib.parse import parse_qs

import core
import payments
import inventory_import
import enterprise

log=logging.getLogger("fvs")
logging.basicConfig(level=os.getenv("FVS_LOG_LEVEL","INFO"))
MAX_BODY=int(os.getenv("FVS_MAX_BODY_BYTES","8388608"))
HOLD_SECONDS=int(os.getenv("FVS_HOLD_SECONDS","900"))
CHECKOUT_HOLD_SECONDS=int(os.getenv("FVS_CHECKOUT_HOLD_SECONDS","1200"))
TERMS_VERSION=os.getenv("FVS_TERMS_VERSION","2026-09-16")
ALLOWED_ORIGINS={x.strip() for x in os.getenv("FVS_ALLOWED_ORIGINS","http://localhost:8080").split(",") if x.strip()}
COOKIE_SECURE=os.getenv("FVS_COOKIE_SECURE","0")=="1"
PUBLIC_BASE=os.getenv("FVS_PUBLIC_BASE_URL","http://localhost:8080").rstrip("/")
API_BASE=os.getenv("FVS_API_BASE_URL",PUBLIC_BASE).rstrip("/")

STATUS_MAP={1:400,2:503,3:404,4:409,5:401,6:409,7:409,8:422,9:500,10:500,11:503}

class WebhookBusy(RuntimeError): pass

def _admin_ok(env):
    supplied=env.get("HTTP_X_ADMIN_KEY","")
    expected_hash=os.getenv("FVS_ADMIN_KEY_SHA256","").strip().lower()
    if expected_hash:
        actual=hashlib.sha256(supplied.encode("utf-8")).hexdigest()
        return secrets.compare_digest(actual,expected_hash)
    if os.getenv("FVS_ENV","development")=="production":
        return False
    expected=os.getenv("FVS_ADMIN_KEY","")
    return bool(expected and secrets.compare_digest(supplied,expected))

def _social_feed_ok(env):
    supplied=env.get("HTTP_X_SOCIAL_FEED_KEY","")
    expected_hash=os.getenv("FVS_SOCIAL_FEED_KEY_SHA256","").strip().lower()
    if not expected_hash:return os.getenv("FVS_ENV","development")!="production"
    return secrets.compare_digest(hashlib.sha256(supplied.encode()).hexdigest(),expected_hash)

def _verify_social_inbound(raw,env):
    secret=os.getenv("FVS_SOCIAL_INBOUND_SECRET","")
    supplied=(env.get("HTTP_X_FVS_SIGNATURE") or "").strip()
    if not secret or not supplied.startswith("sha256="):return False
    expected=hmac.new(secret.encode(),raw,hashlib.sha256).hexdigest()
    return secrets.compare_digest(supplied[7:].lower(),expected.lower())

def _metrics_payload(snap):
    names=(
        ("manual_review_carts","fvs_manual_review_carts","Carts requiring manual payment review"),
        ("manual_review_payments","fvs_manual_review_payments","Payment attempts requiring manual review"),
        ("outbox_dead","fvs_outbox_dead","Dead-letter outbox events"),
        ("outbox_retry_due","fvs_outbox_retry_due","Outbox events currently due for retry"),
        ("outbox_processing_stale","fvs_outbox_processing_stale","Outbox events with expired processing leases"),
        ("webhook_stale","fvs_webhook_stale","Webhook events with expired processing leases"),
        ("holds_expiring_5m","fvs_holds_expiring_5m","Inventory holds expiring within five minutes"),
        ("workers","fvs_workers","Registered worker instances"),
        ("workers_stale","fvs_workers_stale","Worker instances whose heartbeat is stale"),
    )
    out=[]
    for key,name,help_text in names:
        out.extend((f"# HELP {name} {help_text}",f"# TYPE {name} gauge",f"{name} {int(snap.get(key,0) or 0)}"))
    out.extend(("# HELP fvs_ops_degraded Whether the operational snapshot is degraded","# TYPE fvs_ops_degraded gauge",f"fvs_ops_degraded {1 if snap.get('status')!='ok' else 0}"))
    return "\n".join(out)+"\n"

def _text(start,status,text,content_type="text/plain; charset=utf-8",extra=None,cache="no-store"):
    raw=text.encode("utf-8")
    headers=[("Content-Type",content_type),("Cache-Control",cache),("X-Content-Type-Options","nosniff"),("Content-Length",str(len(raw)))]
    start(f"{status} {HTTPStatus(status).phrase}",headers+(extra or []))
    return [raw]

def _public_attempt(a):
    keep=("attempt_id","cart_version","provider","status","requires_confirmation","subtotal_minor","addons_minor","discount_minor","amount_minor","promo_code","source_channel","currency","client_secret","redirect_url","order_id")
    return {k:a.get(k) for k in keep if k in a}

def _headers(extra=None):
    h=[("Content-Type","application/json; charset=utf-8"),("Cache-Control","no-store"),("X-Content-Type-Options","nosniff"),("Referrer-Policy","same-origin"),("X-Frame-Options","DENY"),("Permissions-Policy","camera=(), microphone=(), geolocation=()") ]
    return h+(extra or [])

def _json(start,status,obj,extra=None):
    raw=json.dumps(obj,separators=(",",":"),ensure_ascii=False).encode()
    rid=str(obj.get("request_id") or "") if isinstance(obj,dict) else ""
    rid_h=[("X-Request-ID",rid)] if rid else []
    start(f"{status} {HTTPStatus(status).phrase}",_headers([("Content-Length",str(len(raw)))]+rid_h+(extra or [])))
    return [raw]

def _read_raw(env):
    try:n=int(env.get("CONTENT_LENGTH") or "0")
    except ValueError: raise ValueError("invalid content length")
    if n<0 or n>MAX_BODY: raise OverflowError("request body too large")
    return env["wsgi.input"].read(n) if n else b""

def _read_json(env):
    raw=_read_raw(env) or b"{}"
    return json.loads(raw.decode("utf-8")) if raw else {},raw

def _cookie_secret(env):
    c=SimpleCookie();c.load(env.get("HTTP_COOKIE", ""));m=c.get("fvs_cart_secret");return m.value if m else None

def _support_secret(env):
    c=SimpleCookie();c.load(env.get("HTTP_COOKIE", ""));m=c.get("fvs_support_secret");return m.value if m else None

def _set_cookie(secret):
    parts=[f"fvs_cart_secret={secret}","Path=/api/v1/cart","HttpOnly","SameSite=Lax","Max-Age=86400"]
    if COOKIE_SECURE:parts.append("Secure")
    return ("Set-Cookie","; ".join(parts))

def _set_support_cookie(secret):
    parts=[f"fvs_support_secret={secret}","Path=/api/v1/support","HttpOnly","SameSite=Lax","Max-Age=604800"]
    if COOKIE_SECURE:parts.append("Secure")
    return ("Set-Cookie","; ".join(parts))

def _origin_ok(env):
    origin=env.get("HTTP_ORIGIN")
    return bool(origin and origin in ALLOWED_ORIGINS)

def _require_origin(env):
    if not _origin_ok(env): raise PermissionError("origin not allowed")

def _provider_prepare(attempt, secret):
    if attempt["provider"]=="stripe":
        p=payments.stripe_create_or_resume(attempt)
    else:
        p=payments.mp_create_or_resume(attempt,API_BASE+"/api/v1/webhooks/mercadopago",PUBLIC_BASE+"/?checkout=return")
    core.checkout_attach(attempt["attempt_id"],p.get("checkout_id"),p.get("payment_id"),p.get("client_secret"),p.get("redirect_url"))
    return _public_attempt(core.checkout_get(attempt["cart_id"],secret))

def _stripe_webhook(env,raw):
    secret=os.environ["STRIPE_WEBHOOK_SECRET"]
    payments.stripe_verify_signature(raw,env.get("HTTP_STRIPE_SIGNATURE",""),secret)
    evt=json.loads(raw.decode("utf-8"));event_id=str(evt.get("id") or "")
    if not event_id: raise ValueError("missing event id")
    owner=secrets.token_hex(16);claim,lease_token=core.webhook_claim("stripe",event_id,owner)
    if claim==2: raise WebhookBusy("webhook already being processed")
    if claim==3: return {"received":True,"duplicate":True}
    try:
        if evt.get("type")!="payment_intent.succeeded":
            core.webhook_complete("stripe",event_id,owner,lease_token,True);return {"received":True,"ignored":True}
        pid=str(((evt.get("data") or {}).get("object") or {}).get("id") or "")
        pi=payments.stripe_fetch_payment(pid)
        if pi.get("status")!="succeeded": raise ValueError("payment is not succeeded")
        md=pi.get("metadata") or {};attempt_id=str(md.get("attempt_id") or "");version=int(md.get("cart_version") or 0)
        if not attempt_id or version<=0: raise ValueError("missing FVS metadata")
        amount=int(pi.get("amount_received") or pi.get("amount") or 0);currency=str(pi.get("currency") or "").upper()
        core.checkout_attach(attempt_id,None,pid,None,None)
        result=core.payment_confirm(attempt_id,"stripe",pid,amount,currency,version)
        core.webhook_complete("stripe",event_id,owner,lease_token,True);return {"received":True,"result":result.get("status")}
    except Exception as e:
        log.exception("stripe webhook failed event=%s",event_id)
        try: core.webhook_complete("stripe",event_id,owner,lease_token,False,type(e).__name__)
        except Exception: log.exception("failed releasing stripe webhook lease")
        raise

def _mp_webhook(env,raw,qs):
    payload=json.loads(raw.decode("utf-8")) if raw else {}
    data_id=(qs.get("data.id") or qs.get("data_id") or [None])[0] or str(((payload.get("data") or {}).get("id") or ""))
    request_id=env.get("HTTP_X_REQUEST_ID","")
    payments.mp_verify_signature(env.get("HTTP_X_SIGNATURE",""),request_id,data_id,os.environ["MERCADOPAGO_WEBHOOK_SECRET"])
    event_id=str(payload.get("id") or f"{request_id}:{data_id}")
    owner=secrets.token_hex(16);claim,lease_token=core.webhook_claim("mercadopago",event_id,owner)
    if claim==2: raise WebhookBusy("webhook already being processed")
    if claim==3: return {"received":True,"duplicate":True}
    try:
        payment=payments.mp_fetch_payment(data_id)
        if payment.get("status")!="approved":
            core.webhook_complete("mercadopago",event_id,owner,lease_token,True);return {"received":True,"ignored":True}
        attempt_id=str(payment.get("external_reference") or "")
        md=payment.get("metadata") or {};version=int(md.get("cart_version") or 0)
        if str(md.get("attempt_id") or "")!=attempt_id or not attempt_id or version<=0: raise ValueError("missing FVS metadata")
        amount=payments.decimal_to_minor(payment.get("transaction_amount",0));currency=str(payment.get("currency_id") or "").upper();pid=str(payment.get("id") or data_id)
        core.checkout_attach(attempt_id,None,pid,None,None)
        result=core.payment_confirm(attempt_id,"mercadopago",pid,amount,currency,version)
        core.webhook_complete("mercadopago",event_id,owner,lease_token,True);return {"received":True,"result":result.get("status")}
    except Exception as e:
        log.exception("mercadopago webhook failed event=%s",event_id)
        try: core.webhook_complete("mercadopago",event_id,owner,lease_token,False,type(e).__name__)
        except Exception: log.exception("failed releasing MP webhook lease")
        raise

_REQ_ID_RE=re.compile(r"^[A-Za-z0-9._:-]{1,128}$")
def _request_id(env):
    supplied=(env.get("HTTP_X_REQUEST_ID") or "").strip()
    return supplied if _REQ_ID_RE.fullmatch(supplied) else secrets.token_hex(12)

def application(env,start_response):
    request_id=_request_id(env)
    path=env.get("PATH_INFO","");method=env.get("REQUEST_METHOD","GET").upper();qs=parse_qs(env.get("QUERY_STRING",""),keep_blank_values=True)
    try:
        if path=="/api/v1/health/live" and method=="GET": return _json(start_response,200,{"ok":True,"core":core.version(),"request_id":request_id})
        if path=="/api/v1/health/ready" and method=="GET":
            state=core.schema_status(os.getenv("FVS_SCHEMA_REQUIRED","001_current.sql"));code=200 if state.get("ready") else 503
            return _json(start_response,code,{**state,"request_id":request_id},[] if code==200 else [("Retry-After","5")])
        if path=="/robots.txt" and method=="GET":
            body=f"User-agent: *\nAllow: /\nDisallow: /api/\nDisallow: /admin.html\nDisallow: /support-agent.html\nSitemap: {PUBLIC_BASE}/sitemap.xml\n"
            return _text(start_response,200,body,"text/plain; charset=utf-8",cache="public,max-age=3600")
        if path=="/sitemap.xml" and method=="GET":
            pages=core.seo_pages_list(50000).get("pages",[])
            return _text(start_response,200,enterprise.sitemap_xml(pages),"application/xml; charset=utf-8",cache="public,max-age=300,stale-while-revalidate=3600")
        if path.startswith("/stay/") and method=="GET":
            slug=path[len("/stay/"):].strip("/")
            if not re.fullmatch(r"[a-z0-9-]{1,220}",slug): return _text(start_response,404,"Not found")
            try: page=core.seo_page_get(slug,(qs.get("lang") or ["es-MX"])[0])
            except core.CoreError as e:
                if e.code!=3: raise
                try: redirect=core.seo_redirect_get(path)
                except core.CoreError as redirect_error:
                    if redirect_error.code==3:return _text(start_response,404,"Not found",cache="public,max-age=60")
                    raise
                location=str(redirect.get("target_path") or "/");status=int(redirect.get("status_code") or 301)
                if status not in {301,302,307,308}:status=301
                start_response(f"{status} {HTTPStatus(status).phrase}",[("Location",location),("Cache-Control","public,max-age=3600"),("X-Content-Type-Options","nosniff")]);return [b""]
            return _text(start_response,200,enterprise.seo_html(page),"text/html; charset=utf-8",cache="public,max-age=300,stale-while-revalidate=86400")
        if path=="/api/v1/commerce/home" and method=="GET":
            return _json(start_response,200,{**core.commerce_home(),"request_id":request_id})
        m=re.fullmatch(r"/api/v1/commerce/collections/([A-Za-z0-9_-]{1,120})",path)
        if m and method=="GET":return _json(start_response,200,{**core.commerce_collection_get(m.group(1)),"request_id":request_id})
        if path=="/api/v1/commerce/recommendations" and method=="GET":
            slot=(qs.get("slot_id") or [""])[0];limit=max(1,min(30,int((qs.get("limit") or ["8"])[0])))
            return _json(start_response,200,{**core.commerce_recommendations(slot,limit),"request_id":request_id})
        m=re.fullmatch(r"/s/([0-9a-fA-F]{32})",path)
        if m and method=="GET":
            resolved=core.social_sales_link_resolve(m.group(1));location=str(resolved.get("path") or "/")
            start_response("302 Found",[("Location",location),("Cache-Control","no-store"),("Referrer-Policy","same-origin")]);return [b""]
        m=re.fullmatch(r"/api/v1/social/catalog/([A-Za-z0-9_-]{1,32})",path)
        if m and method=="GET":
            if not _social_feed_ok(env):return _json(start_response,401,{"error":"social_feed_auth","request_id":request_id})
            limit=max(1,min(1000,int((qs.get("limit") or ["500"])[0])));offset=max(0,int((qs.get("offset") or ["0"])[0]))
            return _json(start_response,200,{**core.social_catalog_feed(m.group(1),limit,offset),"request_id":request_id})
        if path=="/api/v1/social/inbound" and method=="POST":
            raw=_read_raw(env)
            if not _verify_social_inbound(raw,env):return _json(start_response,401,{"error":"social_signature","request_id":request_id})
            body=json.loads(raw.decode("utf-8"));result=core.social_lead_capture(body)
            return _json(start_response,202,{**result,"request_id":request_id})
        if path=="/api/v1/support" and method=="POST":
            _require_origin(env);body,_=_read_json(env);tid,secret=core.support_thread_create(str(body.get("email") or ""),str(body.get("subject") or "Ayuda con mi reserva"),str(body.get("cart_id") or ""),str(body.get("order_id") or ""),str(body.get("priority") or "normal"))
            if os.getenv("FVS_SUPPORT_AI_ENABLED","1")!="1": core.support_handoff(tid,"AI support disabled; human handoff")
            return _json(start_response,201,{"thread_id":tid,"status":"open","request_id":request_id},[_set_support_cookie(secret)])
        if path.startswith("/api/v1/support/"):
            if method=="POST": _require_origin(env)
            thread_id=path.split("/")[4] if len(path.split("/"))>4 else "";secret=_support_secret(env)
            if not secret:return _json(start_response,401,{"error":"support_session_missing","request_id":request_id})
            if method=="GET" and re.fullmatch(r"/api/v1/support/[0-9a-fA-F-]{36}",path):
                return _json(start_response,200,{**core.support_thread_get(thread_id,secret),"request_id":request_id})
            if method=="POST" and path.endswith("/messages"):
                body,_=_read_json(env);result=core.support_customer_message(thread_id,secret,str(body.get("body") or ""))
                return _json(start_response,202,{**result,"request_id":request_id})
        if path=="/api/v1/marketing/event" and method=="POST":
            _require_origin(env);body,_=_read_json(env)
            core.marketing_attribution_record(campaign_id=body.get("campaign_id"),channel=body.get("channel"),creative_id=body.get("creative_id"),visitor_id=body.get("visitor_id"),session_id=body.get("session_id"),order_id=body.get("order_id"),event_type=str(body.get("event_type") or "landing"),value_minor=int(body.get("value_minor") or 0),currency=body.get("currency"),utm_source=body.get("utm_source"),utm_medium=body.get("utm_medium"),utm_campaign=body.get("utm_campaign"),utm_content=body.get("utm_content"),referrer=body.get("referrer"))
            return _json(start_response,202,{"accepted":True,"request_id":request_id})
        if path.startswith("/api/v1/webhooks/") and method=="POST":
            raw=_read_raw(env)
            if path.endswith("/stripe"): result=_stripe_webhook(env,raw)
            elif path.endswith("/mercadopago"): result=_mp_webhook(env,raw,qs)
            else:return _json(start_response,404,{"error":"not_found","request_id":request_id})
            return _json(start_response,200,{**result,"request_id":request_id})
        if method in {"POST","PUT","PATCH","DELETE"}: _require_origin(env)
        if path.startswith("/api/v1/agent/"):
            if not enterprise.agent_ok(env): return _json(start_response,401,{"error":"agent_auth","request_id":request_id})
            agent_id=enterprise.agent_id(env)
            if not agent_id:return _json(start_response,400,{"error":"agent_id_required","request_id":request_id})
            if path=="/api/v1/agent/heartbeat" and method=="POST":
                body,_=_read_json(env);core.support_agent_heartbeat(agent_id,str(body.get("display_name") or agent_id),str(body.get("status") or "available"),int(body.get("max_active") or 5));return _json(start_response,204,{})
            if path=="/api/v1/agent/queue" and method=="GET":return _json(start_response,200,{**core.support_queue_list(200),"request_id":request_id})
            m=re.fullmatch(r"/api/v1/agent/threads/([0-9a-fA-F-]{36})(?:/(claim|messages|resolve))?",path)
            if m:
                tid,action=m.group(1),m.group(2)
                if action is None and method=="GET":return _json(start_response,200,{**core.support_thread_context(tid),"request_id":request_id})
                if action=="claim" and method=="POST":core.support_agent_claim(tid,agent_id);return _json(start_response,200,{**core.support_thread_context(tid),"request_id":request_id})
                if action=="messages" and method=="POST":
                    body,_=_read_json(env);return _json(start_response,200,{**core.support_agent_message(tid,agent_id,str(body.get("body") or ""),bool(body.get("internal"))),"request_id":request_id})
                if action=="resolve" and method=="POST":core.support_thread_resolve(tid,agent_id);return _json(start_response,200,{**core.support_thread_context(tid),"request_id":request_id})
        if path=="/api/v1/admin/marketing/campaigns" and method=="GET":
            if not _admin_ok(env):return _json(start_response,401,{"error":"admin_auth","request_id":request_id})
            return _json(start_response,200,{**core.marketing_campaign_list(200),"request_id":request_id})
        if path=="/api/v1/admin/marketing/campaigns" and method=="POST":
            if not _admin_ok(env):return _json(start_response,401,{"error":"admin_auth","request_id":request_id})
            body,_=_read_json(env);actor=(env.get("HTTP_X_ADMIN_ACTOR") or "marketing-admin")[:128]
            r=core.marketing_campaign_create(str(body.get("name") or ""),str(body.get("objective") or "bookings"),str(body.get("automation_mode") or "guarded"),int(body.get("budget_minor") or 0),int(body.get("daily_budget_minor") or 0),str(body.get("currency") or "MXN"),str(body.get("utm_campaign") or ""),actor)
            return _json(start_response,201,{**r,"request_id":request_id})
        mkt=re.fullmatch(r"/api/v1/admin/marketing/campaigns/([0-9a-fA-F-]{36})/approve",path)
        if mkt and method=="POST":
            if not _admin_ok(env):return _json(start_response,401,{"error":"admin_auth","request_id":request_id})
            actor=(env.get("HTTP_X_ADMIN_ACTOR") or "marketing-admin")[:128];core.marketing_campaign_approve(mkt.group(1),actor);return _json(start_response,200,{"status":"approved","request_id":request_id})
        mktcfg=re.fullmatch(r"/api/v1/admin/marketing/campaigns/([0-9a-fA-F-]{36})/configure",path)
        if mktcfg and method=="POST":
            if not _admin_ok(env):return _json(start_response,401,{"error":"admin_auth","request_id":request_id})
            body,_=_read_json(env);actor=(env.get("HTTP_X_ADMIN_ACTOR") or "marketing-admin")[:128]
            core.marketing_campaign_configure(mktcfg.group(1),max_daily_spend_minor=int(body.get("max_daily_spend_minor") or 0),frequency_cap_7d=int(body.get("frequency_cap_7d") or 6),target_roas_bps=int(body.get("target_roas_bps") or 0),stop_loss_minor=int(body.get("stop_loss_minor") or 0),audience=body.get("audience"),geo=body.get("geo"),placements=body.get("placements"),optimization_rules=body.get("optimization_rules"),experiment=body.get("experiment"),actor=actor)
            return _json(start_response,200,{"status":"configured","request_id":request_id})
        mktpause=re.fullmatch(r"/api/v1/admin/marketing/campaigns/([0-9a-fA-F-]{36})/pause",path)
        if mktpause and method=="POST":
            if not _admin_ok(env):return _json(start_response,401,{"error":"admin_auth","request_id":request_id})
            actor=(env.get("HTTP_X_ADMIN_ACTOR") or "marketing-admin")[:128];core.marketing_campaign_pause(mktpause.group(1),actor)
            return _json(start_response,200,{"status":"paused","request_id":request_id})
        if path=="/api/v1/admin/marketing/creatives" and method=="POST":
            if not _admin_ok(env):return _json(start_response,401,{"error":"admin_auth","request_id":request_id})
            body,_=_read_json(env);actor=(env.get("HTTP_X_ADMIN_ACTOR") or "marketing-admin")[:128]
            r=core.marketing_creative_add(str(body.get("campaign_id") or ""),str(body.get("channel") or ""),str(body.get("variant_key") or "A"),str(body.get("headline") or ""),str(body.get("body") or ""),str(body.get("cta") or ""),str(body.get("landing_url") or PUBLIC_BASE),str(body.get("image_url") or ""),bool(body.get("ai_generated")),actor)
            return _json(start_response,201,{**r,"request_id":request_id})
        mktc=re.fullmatch(r"/api/v1/admin/marketing/creatives/([0-9a-fA-F-]{36})/approve",path)
        if mktc and method=="POST":
            if not _admin_ok(env):return _json(start_response,401,{"error":"admin_auth","request_id":request_id})
            actor=(env.get("HTTP_X_ADMIN_ACTOR") or "marketing-admin")[:128];core.marketing_creative_approve(mktc.group(1),actor);return _json(start_response,200,{"status":"approved","request_id":request_id})
        if path=="/api/v1/admin/marketing/jobs" and method=="POST":
            if not _admin_ok(env):return _json(start_response,401,{"error":"admin_auth","request_id":request_id})
            body,_=_read_json(env);actor=(env.get("HTTP_X_ADMIN_ACTOR") or "marketing-admin")[:128]
            r=core.marketing_job_schedule(str(body.get("campaign_id") or ""),str(body.get("creative_id") or ""),str(body.get("channel") or ""),str(body.get("action") or "publish"),str(body.get("scheduled_at") or ""),body.get("payload") or {},actor)
            return _json(start_response,201,{**r,"request_id":request_id})
        if path=="/api/v1/admin/marketing/dashboard" and method=="GET":
            if not _admin_ok(env):return _json(start_response,401,{"error":"admin_auth","request_id":request_id})
            days=max(1,min(730,int((qs.get("days") or ["30"])[0])));return _json(start_response,200,{**core.marketing_dashboard(days),"request_id":request_id})
        if path=="/api/v1/admin/commerce/promotions" and method=="POST":
            if not _admin_ok(env):return _json(start_response,401,{"error":"admin_auth","request_id":request_id})
            body,_=_read_json(env);actor=(env.get("HTTP_X_ADMIN_ACTOR") or "commerce-admin")[:128];core.commerce_promotion_upsert(body,actor)
            return _json(start_response,200,{"status":"upserted","request_id":request_id})
        if path=="/api/v1/admin/commerce/collections" and method=="POST":
            if not _admin_ok(env):return _json(start_response,401,{"error":"admin_auth","request_id":request_id})
            body,_=_read_json(env);actor=(env.get("HTTP_X_ADMIN_ACTOR") or "commerce-admin")[:128];core.commerce_collection_upsert(body,actor)
            return _json(start_response,200,{"status":"upserted","request_id":request_id})
        if path=="/api/v1/admin/social/links" and method=="POST":
            if not _admin_ok(env):return _json(start_response,401,{"error":"admin_auth","request_id":request_id})
            body,_=_read_json(env);actor=(env.get("HTTP_X_ADMIN_ACTOR") or "social-admin")[:128];result=core.social_sales_link_create(body,actor)
            return _json(start_response,201,{**result,"request_id":request_id})
        if path=="/api/v1/admin/seo/rebuild" and method=="POST":
            if not _admin_ok(env):return _json(start_response,401,{"error":"admin_auth","request_id":request_id})
            actor=(env.get("HTTP_X_ADMIN_ACTOR") or "seo-admin")[:128];n=core.seo_rebuild_inventory(actor);return _json(start_response,200,{"upserted":n,"request_id":request_id})
        if path=="/api/v1/admin/seo/pages" and method=="POST":
            if not _admin_ok(env):return _json(start_response,401,{"error":"admin_auth","request_id":request_id})
            body,_=_read_json(env);actor=(env.get("HTTP_X_ADMIN_ACTOR") or "seo-admin")[:128];core.seo_page_upsert(body,actor);return _json(start_response,200,{"status":"upserted","request_id":request_id})
        if path=="/api/v1/admin/ops" and method=="GET":
            if not _admin_ok(env): return _json(start_response,401,{"error":"admin_auth","request_id":request_id})
            snap=core.ops_snapshot(int(os.getenv("FVS_WORKER_STALE_SECONDS","90")))
            return _json(start_response,200,{**snap,"request_id":request_id})
        if path=="/api/v1/admin/metrics" and method=="GET":
            if not _admin_ok(env): return _json(start_response,401,{"error":"admin_auth","request_id":request_id})
            snap=core.ops_snapshot(int(os.getenv("FVS_WORKER_STALE_SECONDS","90")))
            return _text(start_response,200,_metrics_payload(snap),"text/plain; version=0.0.4; charset=utf-8")
        if path=="/api/v1/admin/manual-review" and method=="GET":
            if not _admin_ok(env): return _json(start_response,401,{"error":"admin_auth","request_id":request_id})
            try: limit=max(1,min(200,int((qs.get("limit") or ["100"])[0])))
            except ValueError: return _json(start_response,400,{"error":"bad_request","request_id":request_id})
            result=core.manual_review_list(limit)
            return _json(start_response,200,{**result,"request_id":request_id})
        if path.startswith("/api/v1/admin/outbox/") and path.endswith("/requeue") and method=="POST":
            if not _admin_ok(env): return _json(start_response,401,{"error":"admin_auth","request_id":request_id})
            parts_admin=[p for p in path.split("/") if p]; event_id=parts_admin[-2]
            actor=(env.get("HTTP_X_ADMIN_ACTOR") or "api-admin")[:128]
            core.outbox_requeue_dead(event_id,actor)
            return _json(start_response,202,{"status":"requeued","event_id":event_id,"request_id":request_id})
        if path=="/api/v1/admin/import" and method=="POST":
            if not _admin_ok(env): return _json(start_response,401,{"error":"admin_auth","request_id":request_id})
            raw=_read_raw(env); filename=env.get("HTTP_X_FILENAME","inventory.csv")
            report=inventory_import.import_bytes(raw,filename)
            return _json(start_response,200,{**report,"request_id":request_id})
        if path=="/api/v1/search/suggest" and method=="GET":
            q=(qs.get("q") or [""])[0].strip()
            if len(q)<1:return _json(start_response,200,{"suggestions":[],"request_id":request_id})
            limit=max(1,min(30,int((qs.get("limit") or ["10"])[0])))
            return _json(start_response,200,{**core.inventory_suggest(q,limit),"request_id":request_id})
        if path=="/api/v1/inventory" and method=="GET":
            def qv(name,default=""): return (qs.get(name) or [default])[0]
            def qi(name,default=0):
                try:return int(qv(name,str(default)) or default)
                except ValueError:return default
            bbox=None
            raw_bbox=qv("bbox")
            if raw_bbox:
                try:
                    vals=[float(x) for x in raw_bbox.split(",")]
                    if len(vals)==4:bbox=vals
                except ValueError: bbox=None
            filters={"q":qv("q"),"check_in":qv("check_in"),"check_out":qv("check_out"),"guests":max(1,min(64,qi("guests",2))),
                     "country":qv("country"),"city":qv("city"),"resort":qv("resort"),"property_type":qv("property_type"),"season":qv("season"),"booking_mode":qv("booking_mode"),"amenities":qv("amenities"),
                     "min_bedrooms":max(0,min(40,qi("min_bedrooms",0))),"min_bathrooms":max(0.0,min(40.0,float(qv("min_bathrooms","0") or 0))),"min_rating_x100":max(0,min(500,qi("min_rating_x100",0))),"min_price_minor":max(0,qi("min_price_minor",0)),"max_price_minor":max(0,qi("max_price_minor",0)),"bbox":bbox,"sort":qv("sort","relevance"),"limit":max(1,min(240,qi("limit",60))),"offset":max(0,min(10000,qi("offset",0)))}
            return _json(start_response,200,{**core.inventory_search_v2(filters),"request_id":request_id})
        if path=="/api/v1/cart" and method=="POST":
            cid,secret=core.cart_create();return _json(start_response,201,{"cart_id":cid,"status":"open","request_id":request_id},[_set_cookie(secret)])
        parts=[p for p in path.split("/") if p]
        # api v1 cart <id> [...]
        if len(parts)>=4 and parts[:3]==["api","v1","cart"]:
            cid=parts[3];secret=_cookie_secret(env)
            if not secret:return _json(start_response,401,{"error":"cart_session_missing","request_id":request_id})
            if len(parts)==4 and method=="GET":return _json(start_response,200,{**core.cart_get(cid,secret),"request_id":request_id})
            if len(parts)==5 and parts[4]=="items" and method=="POST":
                body,_=_read_json(env);result=core.cart_add(cid,secret,str(body.get("slot_id","")),int(body.get("guests",1)),HOLD_SECONDS);return _json(start_response,200,{**result,"request_id":request_id})
            if len(parts)==6 and parts[4]=="items" and method=="DELETE":
                result=core.cart_remove(cid,secret,parts[5]);return _json(start_response,200,{**result,"request_id":request_id})
            if len(parts)==5 and parts[4]=="code" and method=="POST":
                body,_=_read_json(env);result=core.cart_apply_code(cid,secret,str(body.get("code") or ""));return _json(start_response,200,{**result,"request_id":request_id})
            if len(parts)==6 and parts[4]=="addons" and method in {"PUT","POST"}:
                body,_=_read_json(env);result=core.cart_addon_set(cid,secret,parts[5],int(body.get("quantity") or 0));return _json(start_response,200,{**result,"request_id":request_id})
            if len(parts)==5 and parts[4]=="origin" and method=="POST":
                body,_=_read_json(env);core.cart_set_origin(cid,secret,str(body.get("channel") or ""),str(body.get("campaign_id") or ""),str(body.get("creative_id") or ""),str(body.get("social_token") or ""));return _json(start_response,200,{**core.cart_get(cid,secret),"request_id":request_id})
            if len(parts)==5 and parts[4]=="checkout" and method=="POST":
                body,_=_read_json(env)
                if body.get("terms_accepted") is not True:return _json(start_response,422,{"error":"terms_required","request_id":request_id})
                attempt=core.checkout_begin(cid,secret,str(body.get("provider","")),str(body.get("email","")),TERMS_VERSION,CHECKOUT_HOLD_SECONDS)
                if attempt.get("status")=="quote_refreshed":return _json(start_response,200,{**_public_attempt(attempt),"request_id":request_id})
                try: result=_provider_prepare(attempt,secret)
                except payments.ProviderError:
                    log.exception("provider prepare failed attempt=%s",attempt.get("attempt_id"));return _json(start_response,202,{"status":"provider_reconciliation_pending","attempt_id":attempt.get("attempt_id"),"request_id":request_id})
                return _json(start_response,200,{**result,"request_id":request_id})
            if len(parts)==6 and parts[4]=="checkout" and parts[5]=="status" and method=="GET":
                attempt=core.checkout_get(cid,secret)
                if attempt.get("status") in {"creating","pending"}:
                    try: attempt=_provider_prepare(attempt,secret)
                    except payments.ProviderError: log.exception("provider resume validation failed attempt=%s",attempt.get("attempt_id"))
                return _json(start_response,200,{**_public_attempt(attempt),"request_id":request_id})
        return _json(start_response,404,{"error":"not_found","request_id":request_id})
    except WebhookBusy:return _json(start_response,503,{"error":"webhook_busy","request_id":request_id},[("Retry-After","2")])
    except PermissionError:return _json(start_response,403,{"error":"forbidden_origin","request_id":request_id})
    except OverflowError:return _json(start_response,413,{"error":"payload_too_large","request_id":request_id})
    except (ValueError,json.JSONDecodeError):return _json(start_response,400,{"error":"bad_request","request_id":request_id})
    except core.CoreError as e:
        log.warning("core error request=%s code=%s detail=%s",request_id,e.code,e)
        return _json(start_response,STATUS_MAP.get(e.code,500),{"error":core.lib.fvs_status_name(e.code).decode() if hasattr(core.lib,"fvs_status_name") else "core_error","request_id":request_id},[("Retry-After","1")] if e.code==11 else None)
    except payments.ProviderError:
        log.exception("provider error request=%s",request_id);return _json(start_response,502,{"error":"payment_provider_error","request_id":request_id})
    except Exception:
        log.error("unhandled request=%s\n%s",request_id,traceback.format_exc());return _json(start_response,500,{"error":"internal_error","request_id":request_id})
