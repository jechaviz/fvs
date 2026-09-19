"""ctypes binding for libfvs_core. One native context per thread."""
from __future__ import annotations

import ctypes
import json
import os
import threading
from pathlib import Path

BUF_SIZE = 4 * 1024 * 1024

class CoreError(RuntimeError):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code


def _lib_path() -> str:
    explicit = os.getenv("FVS_CORE_LIB")
    if explicit:
        return explicit
    candidates = [
        Path(__file__).resolve().parents[1] / "core" / "build" / "release" / "libfvs_core.so",
        Path("/usr/local/lib/libfvs_core.so"),
        Path("/usr/lib/libfvs_core.so"),
    ]
    for p in candidates:
        if p.exists():
            return str(p)
    return "libfvs_core.so"

lib = ctypes.CDLL(_lib_path())
ctx_p = ctypes.c_void_p
size_t = ctypes.c_size_t

lib.fvs_ctx_open.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
lib.fvs_ctx_open.restype = ctx_p
lib.fvs_ctx_close.argtypes = [ctx_p]
lib.fvs_last_error.argtypes = [ctx_p]
lib.fvs_last_error.restype = ctypes.c_char_p
lib.fvs_version.restype = ctypes.c_char_p
lib.fvs_status_name.argtypes = [ctypes.c_int]
lib.fvs_status_name.restype = ctypes.c_char_p
lib.fvs_ping.argtypes = [ctx_p]
lib.fvs_ping.restype = ctypes.c_int

for name, args in {
    "fvs_inventory_search": [ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_char_p,size_t],
    "fvs_cart_get": [ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,size_t],
    "fvs_cart_add": [ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_uint,ctypes.c_char_p,size_t],
    "fvs_cart_remove": [ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,size_t],
    "fvs_checkout_begin": [ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_char_p,size_t],
    "fvs_checkout_get": [ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,size_t],
    "fvs_payment_confirm": [ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_longlong,ctypes.c_char_p,ctypes.c_ulonglong,ctypes.c_char_p,size_t],
    "fvs_outbox_claim": [ctx_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_char_p,size_t],
    "fvs_order_get": [ctx_p,ctypes.c_char_p,ctypes.c_char_p,size_t],
}.items():
    fn = getattr(lib, name); fn.argtypes = args; fn.restype = ctypes.c_int

lib.fvs_inventory_search_v2.argtypes = [ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_double,ctypes.c_uint,ctypes.c_int64,ctypes.c_int64,ctypes.c_double,ctypes.c_double,ctypes.c_double,ctypes.c_double,ctypes.c_int,ctypes.c_char_p,ctypes.c_uint,ctypes.c_uint,ctypes.c_char_p,size_t]; lib.fvs_inventory_search_v2.restype = ctypes.c_int
lib.fvs_inventory_suggest.argtypes = [ctx_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_char_p,size_t]; lib.fvs_inventory_suggest.restype = ctypes.c_int
lib.fvs_inventory_discovery_meta_upsert.argtypes = [ctx_p,ctypes.c_char_p,ctypes.c_int,ctypes.c_double,ctypes.c_double,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_double,ctypes.c_uint]; lib.fvs_inventory_discovery_meta_upsert.restype = ctypes.c_int
lib.fvs_inventory_upsert_v2.argtypes = [ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_longlong,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int,ctypes.POINTER(ctypes.c_int)]; lib.fvs_inventory_upsert_v2.restype = ctypes.c_int
lib.fvs_inventory_upsert.argtypes = [ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_longlong,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int,ctypes.POINTER(ctypes.c_int)]; lib.fvs_inventory_upsert.restype = ctypes.c_int
lib.fvs_cart_create.argtypes = [ctx_p,ctypes.c_char_p,size_t,ctypes.c_char_p,size_t]; lib.fvs_cart_create.restype = ctypes.c_int
lib.fvs_checkout_attach_provider.argtypes = [ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p]; lib.fvs_checkout_attach_provider.restype = ctypes.c_int
lib.fvs_webhook_claim_v2.argtypes = [ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_char_p,size_t,ctypes.POINTER(ctypes.c_int)]; lib.fvs_webhook_claim_v2.restype = ctypes.c_int
lib.fvs_webhook_complete_v2.argtypes = [ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int,ctypes.c_char_p]; lib.fvs_webhook_complete_v2.restype = ctypes.c_int
lib.fvs_outbox_ack.argtypes = [ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p]; lib.fvs_outbox_ack.restype = ctypes.c_int
lib.fvs_outbox_nack.argtypes = [ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p]; lib.fvs_outbox_nack.restype = ctypes.c_int
lib.fvs_maintenance_sweep.argtypes = [ctx_p,ctypes.c_uint,ctypes.POINTER(ctypes.c_uint),ctypes.POINTER(ctypes.c_uint)]; lib.fvs_maintenance_sweep.restype = ctypes.c_int
lib.fvs_schema_status.argtypes = [ctx_p,ctypes.c_char_p,ctypes.c_char_p,size_t]; lib.fvs_schema_status.restype = ctypes.c_int
lib.fvs_ops_snapshot.argtypes = [ctx_p,ctypes.c_uint,ctypes.c_char_p,size_t]; lib.fvs_ops_snapshot.restype = ctypes.c_int
lib.fvs_worker_heartbeat.argtypes = [ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p]; lib.fvs_worker_heartbeat.restype = ctypes.c_int
lib.fvs_worker_goodbye.argtypes = [ctx_p,ctypes.c_char_p,ctypes.c_char_p]; lib.fvs_worker_goodbye.restype = ctypes.c_int
lib.fvs_outbox_requeue_dead.argtypes = [ctx_p,ctypes.c_char_p,ctypes.c_char_p]; lib.fvs_outbox_requeue_dead.restype = ctypes.c_int
lib.fvs_manual_review_list.argtypes = [ctx_p,ctypes.c_uint,ctypes.c_char_p,size_t]; lib.fvs_manual_review_list.restype = ctypes.c_int
lib.fvs_ops_action_record.argtypes = [ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p]; lib.fvs_ops_action_record.restype = ctypes.c_int

_tls = threading.local()

class _ContextHolder:
    __slots__=("ptr",)
    def __init__(self, ptr): self.ptr=ptr
    def close(self):
        p=self.ptr
        if p:
            try: lib.fvs_ctx_close(p)
            finally: self.ptr=None
    def __del__(self):
        try: self.close()
        except Exception: pass

def b(s: str | None) -> bytes | None:
    return None if s is None else s.encode("utf-8")

def ctx():
    holder = getattr(_tls, "holder", None)
    if holder and holder.ptr:
        if lib.fvs_ping(holder.ptr) == 0:
            return holder.ptr
        holder.close()
        _tls.holder = None
    c = lib.fvs_ctx_open(
        b(os.getenv("FVS_DB_HOST", "127.0.0.1")),
        int(os.getenv("FVS_DB_PORT", "3306")),
        b(os.getenv("FVS_DB_USER", "fvs")),
        b(os.getenv("FVS_DB_PASSWORD", "")),
        b(os.getenv("FVS_DB_NAME", "fvs")),
        int(os.getenv("FVS_DB_CONNECT_TIMEOUT", "5")),
    )
    if not c:
        raise CoreError(2, "database connection failed")
    _tls.holder = _ContextHolder(c)
    return c

def close_thread_ctx():
    holder=getattr(_tls,"holder",None)
    if holder: holder.close()
    _tls.holder=None

def _err(c, code: int):
    raw = lib.fvs_last_error(c)
    msg = raw.decode("utf-8", "replace") if raw else "core error"
    raise CoreError(code, msg)

def _json_call(fn, *args):
    c = ctx(); out = ctypes.create_string_buffer(BUF_SIZE)
    rc = fn(c, *args, out, BUF_SIZE)
    if rc: _err(c, rc)
    return json.loads(out.value.decode("utf-8"))

def version(): return lib.fvs_version().decode()
def search_ranking_version(): return lib.fvs_search_ranking_version().decode()
def ping():
    c=ctx(); rc=lib.fvs_ping(c)
    if rc: _err(c,rc)
    return True

def inventory_search(check_in, check_out, guests): return _json_call(lib.fvs_inventory_search,b(check_in),b(check_out),guests)
def inventory_search_v2(filters):
    bbox=filters.get("bbox") or None
    use_bbox=1 if bbox and len(bbox)==4 else 0
    if not use_bbox: bbox=[0.0,0.0,0.0,0.0]
    result=_json_call(lib.fvs_inventory_search_v2,
        b(str(filters.get("q") or "")), b(str(filters.get("check_in") or "")), b(str(filters.get("check_out") or "")), int(filters.get("guests") or 2),
        b(str(filters.get("country") or "")), b(str(filters.get("city") or "")), b(str(filters.get("resort") or "")), b(str(filters.get("property_type") or "")),
        b(str(filters.get("season") or "")), b(str(filters.get("booking_mode") or "")), b(str(filters.get("amenities") or "")),
        int(filters.get("min_bedrooms") or 0), float(filters.get("min_bathrooms") or 0), int(filters.get("min_rating_x100") or 0),
        int(filters.get("min_price_minor") or 0), int(filters.get("max_price_minor") or 0),
        float(bbox[0]),float(bbox[1]),float(bbox[2]),float(bbox[3]),use_bbox,b(str(filters.get("sort") or "relevance")),int(filters.get("limit") or 60),int(filters.get("offset") or 0))
    if int(filters.get("offset") or 0)==0:
        _commerce_event_safe(event_type="search",source="server",query_text=str(filters.get("q") or "")[:300],result_count=int(result.get("total") or len(result.get("items") or [])),metadata_json={"sort":str(filters.get("sort") or "relevance"),"country":str(filters.get("country") or ""), "city":str(filters.get("city") or ""), "ranking_version":search_ranking_version()})
    return result
def inventory_suggest(query,limit=10): return _json_call(lib.fvs_inventory_suggest,b(query),int(limit))
def inventory_upsert(row):
    c=ctx();created=ctypes.c_int()
    rc=lib.fvs_inventory_upsert_v2(c,b(row["source_ref"]),b(row["resort"]),b(row["unit_code"]),b(row["unit_name"]),b(row.get("city","")),b(row.get("country","")),b(row["check_in"]),b(row["check_out"]),int(row.get("week_number") or 0),b(row.get("booking_mode","fixed")),b(row.get("float_group","")),int(row["max_guests"]),int(row["price_minor"]),b(row["currency"]),b(row.get("image_url","")),b(row.get("short_description","")),b(row.get("season","")),1 if row.get("active",True) else 0,ctypes.byref(created))
    if rc:_err(c,rc)
    amenities=row.get("amenities") or []
    if isinstance(amenities,str): amenities=[x.strip() for x in amenities.replace(";",",").split(",") if x.strip()]
    amenities_json=json.dumps(amenities,ensure_ascii=False,separators=(",",":"))
    lat=row.get("latitude");lng=row.get("longitude");has_geo=1 if lat not in (None,"") and lng not in (None,"") else 0
    rc=lib.fvs_inventory_discovery_meta_upsert(c,b(row["source_ref"]),has_geo,float(lat or 0),float(lng or 0),b(row.get("property_type", "")),b(amenities_json),int(row.get("bedrooms") or 0),float(row.get("bathrooms") or 0),int(round(float(row.get("rating") or 0)*100)))
    if rc:_err(c,rc)
    return bool(created.value)
def cart_create():
    c=ctx(); cid=ctypes.create_string_buffer(37); sec=ctypes.create_string_buffer(65)
    rc=lib.fvs_cart_create(c,cid,37,sec,65)
    if rc:_err(c,rc)
    return cid.value.decode(),sec.value.decode()
def cart_get(cid,secret): return _json_call(lib.fvs_cart_get,b(cid),b(secret))
def cart_add(cid,secret,slot_id,guests,hold=900):
    result=_json_call(lib.fvs_cart_add,b(cid),b(secret),b(slot_id),guests,hold)
    _commerce_event_safe(event_key=f"cart_add:{cid}:v{result.get('version',0)}:{slot_id}",event_type="cart_add",source="server",cart_id=cid,slot_id=slot_id,channel=result.get("source_channel"),outcome="held",value_minor=int(result.get("total_minor") or 0),currency=result.get("currency"),metadata_json={"guests":int(guests)})
    return result
def cart_remove(cid,secret,slot_id): return _json_call(lib.fvs_cart_remove,b(cid),b(secret),b(slot_id))
def checkout_begin(cid,secret,provider,email,terms,hold=1200):
    result=_json_call(lib.fvs_checkout_begin,b(cid),b(secret),b(provider),b(email),b(terms),hold)
    attempt_id=result.get("attempt_id") or ""
    outcome="requote" if result.get("status")=="quote_refreshed" else "started"
    _commerce_event_safe(event_key=(f"checkout:{attempt_id}" if attempt_id else ""),event_type="checkout_start",source="server",cart_id=cid,attempt_id=attempt_id,provider=provider,outcome=outcome,value_minor=int(result.get("amount_minor") or result.get("total_minor") or 0),currency=result.get("currency"),reason_code=("quote_refreshed" if outcome=="requote" else ""))
    if attempt_id: payment_recovery_mark(attempt_id,"active","checkout_started","",0)
    return result
def checkout_get(cid,secret):
    result=_json_call(lib.fvs_checkout_get,b(cid),b(secret))
    aid=result.get("attempt_id") or ""
    if aid:
        try: result["recovery"]=payment_recovery_get(aid)
        except CoreError as exc:
            if exc.code!=3: raise
    return result
def checkout_attach(attempt_id,checkout_id=None,payment_id=None,client_secret=None,redirect_url=None):
    c=ctx();rc=lib.fvs_checkout_attach_provider(c,b(attempt_id),b(checkout_id),b(payment_id),b(client_secret),b(redirect_url))
    if rc:_err(c,rc)
def payment_confirm(attempt_id,provider,payment_id,amount_minor,currency,version):
    result=_json_call(lib.fvs_payment_confirm,b(attempt_id),b(provider),b(payment_id),amount_minor,b(currency),version)
    status=str(result.get("status") or "")
    if status=="confirmed" and result.get("order_id"):
        _commerce_event_safe(event_key=f"booking:{result['order_id']}",event_type="booking",source="provider",attempt_id=attempt_id,order_id=result.get("order_id"),provider=provider,outcome="confirmed",value_minor=int(result.get("total_minor") or amount_minor),currency=result.get("currency") or currency)
        payment_recovery_mark(attempt_id,"resolved","payment_confirmed","",0)
    elif status=="manual_review":
        _commerce_event_safe(event_key=f"payment:{attempt_id}:manual_review",event_type="payment",source="provider",attempt_id=attempt_id,provider=provider,outcome="manual_review",reason_code="reconciliation")
        payment_recovery_mark(attempt_id,"manual_review","reconciliation","",0)
    return result
def webhook_claim(provider,event_id,lease_owner,ttl=120):
    c=ctx();out=ctypes.c_int();token=ctypes.create_string_buffer(33)
    rc=lib.fvs_webhook_claim_v2(c,b(provider),b(event_id),b(lease_owner),ttl,token,33,ctypes.byref(out))
    if rc:_err(c,rc)
    return out.value,token.value.decode()
def webhook_complete(provider,event_id,lease_owner,lease_token,success,error=None):
    c=ctx();rc=lib.fvs_webhook_complete_v2(c,b(provider),b(event_id),b(lease_owner),b(lease_token),1 if success else 0,b(error))
    if rc:_err(c,rc)
def outbox_claim(owner,ttl=120): return _json_call(lib.fvs_outbox_claim,b(owner),ttl)
def outbox_ack(event_id,owner,lease_token):
    c=ctx();rc=lib.fvs_outbox_ack(c,b(event_id),b(owner),b(lease_token));
    if rc:_err(c,rc)
def outbox_nack(event_id,owner,lease_token,error):
    c=ctx();rc=lib.fvs_outbox_nack(c,b(event_id),b(owner),b(lease_token),b(error));
    if rc:_err(c,rc)
def order_get(order_id): return _json_call(lib.fvs_order_get,b(order_id))
def maintenance_sweep(open_cart_ttl=86400):
    c=ctx();expired=ctypes.c_uint();released=ctypes.c_uint();rc=lib.fvs_maintenance_sweep(c,int(open_cart_ttl),ctypes.byref(expired),ctypes.byref(released))
    if rc:_err(c,rc)
    return {"expired_carts":int(expired.value),"released_holds":int(released.value)}

def schema_status(required="001_current.sql"):
    return _json_call(lib.fvs_schema_status,b(required))
def ops_snapshot(stale_worker_seconds=90):
    return _json_call(lib.fvs_ops_snapshot,int(stale_worker_seconds))
def worker_heartbeat(worker_name,instance_id,worker_version=None):
    c=ctx(); rc=lib.fvs_worker_heartbeat(c,b(worker_name),b(instance_id),b(worker_version or version()))
    if rc:_err(c,rc)
def worker_goodbye(worker_name,instance_id):
    c=ctx(); rc=lib.fvs_worker_goodbye(c,b(worker_name),b(instance_id))
    if rc:_err(c,rc)
def outbox_requeue_dead(event_id,actor):
    c=ctx(); rc=lib.fvs_outbox_requeue_dead(c,b(event_id),b(actor))
    if rc:_err(c,rc)
def manual_review_list(limit=100):
    return _json_call(lib.fvs_manual_review_list,int(limit))
def ops_action_record(actor,action_type,target_id):
    c=ctx(); rc=lib.fvs_ops_action_record(c,b(actor),b(action_type),b(target_id))
    if rc:_err(c,rc)

# Enterprise support/growth/SEO bindings.
lib.fvs_support_thread_create.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,size_t,ctypes.c_char_p,size_t];lib.fvs_support_thread_create.restype=ctypes.c_int
lib.fvs_support_thread_get.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,size_t];lib.fvs_support_thread_get.restype=ctypes.c_int
lib.fvs_support_thread_context.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,size_t];lib.fvs_support_thread_context.restype=ctypes.c_int
lib.fvs_support_customer_message.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,size_t];lib.fvs_support_customer_message.restype=ctypes.c_int
lib.fvs_support_ai_message.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_uint,ctypes.c_char_p,size_t];lib.fvs_support_ai_message.restype=ctypes.c_int
lib.fvs_support_handoff.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p];lib.fvs_support_handoff.restype=ctypes.c_int
lib.fvs_support_agent_heartbeat.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint];lib.fvs_support_agent_heartbeat.restype=ctypes.c_int
lib.fvs_support_queue_list.argtypes=[ctx_p,ctypes.c_uint,ctypes.c_char_p,size_t];lib.fvs_support_queue_list.restype=ctypes.c_int
lib.fvs_support_agent_claim.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p];lib.fvs_support_agent_claim.restype=ctypes.c_int
lib.fvs_support_agent_message.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int,ctypes.c_char_p,size_t];lib.fvs_support_agent_message.restype=ctypes.c_int
lib.fvs_support_thread_resolve.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p];lib.fvs_support_thread_resolve.restype=ctypes.c_int
lib.fvs_marketing_campaign_create.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_longlong,ctypes.c_longlong,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,size_t];lib.fvs_marketing_campaign_create.restype=ctypes.c_int
lib.fvs_marketing_campaign_list.argtypes=[ctx_p,ctypes.c_uint,ctypes.c_char_p,size_t];lib.fvs_marketing_campaign_list.restype=ctypes.c_int
lib.fvs_marketing_campaign_approve.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p];lib.fvs_marketing_campaign_approve.restype=ctypes.c_int
lib.fvs_marketing_campaign_configure.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_longlong,ctypes.c_uint,ctypes.c_uint,ctypes.c_longlong,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p];lib.fvs_marketing_campaign_configure.restype=ctypes.c_int
lib.fvs_marketing_campaign_pause.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p];lib.fvs_marketing_campaign_pause.restype=ctypes.c_int
lib.fvs_marketing_creative_add.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int,ctypes.c_char_p,ctypes.c_char_p,size_t];lib.fvs_marketing_creative_add.restype=ctypes.c_int
lib.fvs_marketing_creative_approve.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p];lib.fvs_marketing_creative_approve.restype=ctypes.c_int
lib.fvs_marketing_job_schedule.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,size_t];lib.fvs_marketing_job_schedule.restype=ctypes.c_int
lib.fvs_marketing_job_claim.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_char_p,size_t];lib.fvs_marketing_job_claim.restype=ctypes.c_int
lib.fvs_marketing_job_ack.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p];lib.fvs_marketing_job_ack.restype=ctypes.c_int
lib.fvs_marketing_job_nack.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p];lib.fvs_marketing_job_nack.restype=ctypes.c_int
lib.fvs_marketing_metric_upsert.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_ulonglong,ctypes.c_ulonglong,ctypes.c_longlong,ctypes.c_ulonglong,ctypes.c_ulonglong,ctypes.c_longlong,ctypes.c_char_p];lib.fvs_marketing_metric_upsert.restype=ctypes.c_int
lib.fvs_marketing_attribution_record.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_longlong,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p];lib.fvs_marketing_attribution_record.restype=ctypes.c_int
lib.fvs_marketing_dashboard.argtypes=[ctx_p,ctypes.c_uint,ctypes.c_char_p,size_t];lib.fvs_marketing_dashboard.restype=ctypes.c_int
lib.fvs_experiment_snapshot.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_char_p,size_t];lib.fvs_experiment_snapshot.restype=ctypes.c_int
lib.fvs_experiment_dashboard.argtypes=[ctx_p,ctypes.c_uint,ctypes.c_char_p,size_t];lib.fvs_experiment_dashboard.restype=ctypes.c_int
lib.fvs_search_ranking_version.argtypes=[];lib.fvs_search_ranking_version.restype=ctypes.c_char_p
lib.fvs_search_eval_record.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_uint,ctypes.c_uint,ctypes.c_uint,ctypes.c_char_p,size_t];lib.fvs_search_eval_record.restype=ctypes.c_int
lib.fvs_search_quality_dashboard.argtypes=[ctx_p,ctypes.c_uint,ctypes.c_char_p,size_t];lib.fvs_search_quality_dashboard.restype=ctypes.c_int
lib.fvs_payment_recovery_mark.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint];lib.fvs_payment_recovery_mark.restype=ctypes.c_int
lib.fvs_payment_recovery_get.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,size_t];lib.fvs_payment_recovery_get.restype=ctypes.c_int
lib.fvs_payment_recovery_dashboard.argtypes=[ctx_p,ctypes.c_uint,ctypes.c_char_p,size_t];lib.fvs_payment_recovery_dashboard.restype=ctypes.c_int
lib.fvs_commerce_event_record.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_longlong,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int,ctypes.c_char_p];lib.fvs_commerce_event_record.restype=ctypes.c_int
lib.fvs_commerce_funnel_dashboard.argtypes=[ctx_p,ctypes.c_uint,ctypes.c_char_p,size_t];lib.fvs_commerce_funnel_dashboard.restype=ctypes.c_int
lib.fvs_commerce_home.argtypes=[ctx_p,ctypes.c_char_p,size_t];lib.fvs_commerce_home.restype=ctypes.c_int
lib.fvs_commerce_collection_get.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,size_t];lib.fvs_commerce_collection_get.restype=ctypes.c_int
lib.fvs_commerce_recommendations.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_char_p,size_t];lib.fvs_commerce_recommendations.restype=ctypes.c_int
lib.fvs_cart_apply_code.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,size_t];lib.fvs_cart_apply_code.restype=ctypes.c_int
lib.fvs_cart_addon_set.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_char_p,size_t];lib.fvs_cart_addon_set.restype=ctypes.c_int
lib.fvs_cart_set_origin.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p];lib.fvs_cart_set_origin.restype=ctypes.c_int
lib.fvs_commerce_promotion_upsert.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_longlong,ctypes.c_longlong,ctypes.c_longlong,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_ulonglong,ctypes.c_int,ctypes.c_char_p,ctypes.c_char_p];lib.fvs_commerce_promotion_upsert.restype=ctypes.c_int
lib.fvs_commerce_collection_upsert.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int,ctypes.c_int,ctypes.c_char_p];lib.fvs_commerce_collection_upsert.restype=ctypes.c_int
lib.fvs_social_catalog_feed.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_uint,ctypes.c_char_p,size_t];lib.fvs_social_catalog_feed.restype=ctypes.c_int
lib.fvs_social_sales_link_create.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,size_t];lib.fvs_social_sales_link_create.restype=ctypes.c_int
lib.fvs_social_sales_link_resolve.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,size_t];lib.fvs_social_sales_link_resolve.restype=ctypes.c_int
lib.fvs_social_lead_capture.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,size_t];lib.fvs_social_lead_capture.restype=ctypes.c_int
lib.fvs_seo_rebuild_inventory.argtypes=[ctx_p,ctypes.c_char_p,ctypes.POINTER(ctypes.c_uint)];lib.fvs_seo_rebuild_inventory.restype=ctypes.c_int
lib.fvs_seo_page_upsert.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int,ctypes.c_char_p];lib.fvs_seo_page_upsert.restype=ctypes.c_int
lib.fvs_seo_page_get.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,size_t];lib.fvs_seo_page_get.restype=ctypes.c_int
lib.fvs_seo_pages_list.argtypes=[ctx_p,ctypes.c_uint,ctypes.c_char_p,size_t];lib.fvs_seo_pages_list.restype=ctypes.c_int
lib.fvs_seo_redirect_get.argtypes=[ctx_p,ctypes.c_char_p,ctypes.c_char_p,size_t];lib.fvs_seo_redirect_get.restype=ctypes.c_int

def support_thread_create(email,subject,cart_id=None,order_id=None,priority='normal'):
    c=ctx(); tid=ctypes.create_string_buffer(37); sec=ctypes.create_string_buffer(65)
    rc=lib.fvs_support_thread_create(c,b(email or ''),b(subject),b(cart_id or ''),b(order_id or ''),b(priority),tid,37,sec,65)
    if rc:_err(c,rc)
    return tid.value.decode(),sec.value.decode()
def support_thread_get(thread_id,secret): return _json_call(lib.fvs_support_thread_get,b(thread_id),b(secret))
def support_thread_context(thread_id): return _json_call(lib.fvs_support_thread_context,b(thread_id))
def support_customer_message(thread_id,secret,body): return _json_call(lib.fvs_support_customer_message,b(thread_id),b(secret),b(body))
def support_ai_message(thread_id,body,model='',input_tokens=0,output_tokens=0): return _json_call(lib.fvs_support_ai_message,b(thread_id),b(body),b(model),int(input_tokens),int(output_tokens))
def support_handoff(thread_id,reason):
    c=ctx();rc=lib.fvs_support_handoff(c,b(thread_id),b(reason))
    if rc:_err(c,rc)
def support_agent_heartbeat(agent_id,name,status='available',max_active=5):
    c=ctx();rc=lib.fvs_support_agent_heartbeat(c,b(agent_id),b(name),b(status),int(max_active))
    if rc:_err(c,rc)
def support_queue_list(limit=100): return _json_call(lib.fvs_support_queue_list,int(limit))
def support_agent_claim(thread_id,agent_id):
    c=ctx();rc=lib.fvs_support_agent_claim(c,b(thread_id),b(agent_id))
    if rc:_err(c,rc)
def support_agent_message(thread_id,agent_id,body,internal=False): return _json_call(lib.fvs_support_agent_message,b(thread_id),b(agent_id),b(body),1 if internal else 0)
def support_thread_resolve(thread_id,agent_id):
    c=ctx();rc=lib.fvs_support_thread_resolve(c,b(thread_id),b(agent_id))
    if rc:_err(c,rc)

def marketing_campaign_create(name,objective,automation_mode,budget_minor,daily_budget_minor,currency,utm_campaign,actor):
    return _json_call(lib.fvs_marketing_campaign_create,b(name),b(objective),b(automation_mode),int(budget_minor),int(daily_budget_minor),b(currency),b(utm_campaign),b(actor))
def marketing_campaign_list(limit=100): return _json_call(lib.fvs_marketing_campaign_list,int(limit))
def marketing_campaign_approve(campaign_id,actor):
    c=ctx();rc=lib.fvs_marketing_campaign_approve(c,b(campaign_id),b(actor))
    if rc:_err(c,rc)
def marketing_campaign_configure(campaign_id,max_daily_spend_minor=0,frequency_cap_7d=6,target_roas_bps=0,stop_loss_minor=0,audience=None,geo=None,placements=None,optimization_rules=None,experiment=None,actor='marketing-admin'):
    c=ctx(); dumps=lambda v: json.dumps(v or {},ensure_ascii=False,separators=(',',':'))
    rc=lib.fvs_marketing_campaign_configure(c,b(campaign_id),int(max_daily_spend_minor),int(frequency_cap_7d),int(target_roas_bps),int(stop_loss_minor),b(dumps(audience)),b(dumps(geo)),b(dumps(placements)),b(dumps(optimization_rules)),b(dumps(experiment)),b(actor))
    if rc:_err(c,rc)
def marketing_campaign_pause(campaign_id,actor):
    c=ctx();rc=lib.fvs_marketing_campaign_pause(c,b(campaign_id),b(actor))
    if rc:_err(c,rc)
def marketing_creative_add(campaign_id,channel,variant_key,headline,body,cta,landing_url,image_url,ai_generated,actor):
    return _json_call(lib.fvs_marketing_creative_add,b(campaign_id),b(channel),b(variant_key),b(headline),b(body),b(cta or ''),b(landing_url),b(image_url or ''),1 if ai_generated else 0,b(actor))
def marketing_creative_approve(creative_id,actor):
    c=ctx();rc=lib.fvs_marketing_creative_approve(c,b(creative_id),b(actor))
    if rc:_err(c,rc)
def marketing_job_schedule(campaign_id,creative_id,channel,action,scheduled_at,payload,actor):
    return _json_call(lib.fvs_marketing_job_schedule,b(campaign_id),b(creative_id or ''),b(channel),b(action),b(scheduled_at),b(json.dumps(payload or {},separators=(',',':'))),b(actor))
def marketing_job_claim(owner,ttl=120): return _json_call(lib.fvs_marketing_job_claim,b(owner),int(ttl))
def marketing_job_ack(job_id,owner,token,provider_ref=''):
    c=ctx();rc=lib.fvs_marketing_job_ack(c,b(job_id),b(owner),b(token),b(provider_ref or ''))
    if rc:_err(c,rc)
def marketing_job_nack(job_id,owner,token,error):
    c=ctx();rc=lib.fvs_marketing_job_nack(c,b(job_id),b(owner),b(token),b(error))
    if rc:_err(c,rc)
def marketing_metric_upsert(campaign_id,channel,metric_date,impressions,clicks,spend_minor,leads,bookings,revenue_minor,currency):
    c=ctx();rc=lib.fvs_marketing_metric_upsert(c,b(campaign_id),b(channel),b(metric_date),int(impressions),int(clicks),int(spend_minor),int(leads),int(bookings),int(revenue_minor),b(currency))
    if rc:_err(c,rc)
def marketing_attribution_record(**v):
    c=ctx();rc=lib.fvs_marketing_attribution_record(c,b(v.get('campaign_id') or ''),b(v.get('channel') or ''),b(v.get('creative_id') or ''),b(v.get('visitor_id') or ''),b(v.get('session_id') or ''),b(v.get('order_id') or ''),b(v['event_type']),int(v.get('value_minor') or 0),b(v.get('currency') or ''),b(v.get('utm_source') or ''),b(v.get('utm_medium') or ''),b(v.get('utm_campaign') or ''),b(v.get('utm_content') or ''),b(v.get('referrer') or ''))
    if rc:_err(c,rc)
def experiment_snapshot(campaign_id,days=30): return _json_call(lib.fvs_experiment_snapshot,b(campaign_id),int(days))
def experiment_dashboard(days=30): return _json_call(lib.fvs_experiment_dashboard,int(days))

def search_eval_record(ranking_version,corpus_sha256,mrr,ndcg10,precision10,query_count):
    ppm=lambda x:max(0,min(1000000,int(round(float(x)*1000000))))
    return _json_call(lib.fvs_search_eval_record,b(ranking_version),b(corpus_sha256),ppm(mrr),ppm(ndcg10),ppm(precision10),int(query_count))
def search_quality_dashboard(days=30): return _json_call(lib.fvs_search_quality_dashboard,int(days))

def payment_recovery_mark(attempt_id,state,reason_code="",last_error="",retry_after_seconds=0):
    c=ctx();rc=lib.fvs_payment_recovery_mark(c,b(attempt_id),b(state),b(reason_code),b(last_error),int(retry_after_seconds))
    if rc:_err(c,rc)
def payment_recovery_get(attempt_id): return _json_call(lib.fvs_payment_recovery_get,b(attempt_id))
def payment_recovery_dashboard(days=30): return _json_call(lib.fvs_payment_recovery_dashboard,int(days))

def commerce_event_record(**v):
    metadata=v.get("metadata_json")
    if isinstance(metadata,(dict,list)): metadata=json.dumps(metadata,ensure_ascii=False,separators=(",",":"))
    c=ctx();rc=lib.fvs_commerce_event_record(c,b(v.get("event_key") or ""),b(v["event_type"]),b(v.get("source") or "server"),b(v.get("visitor_id") or ""),b(v.get("session_id") or ""),b(v.get("cart_id") or ""),b(v.get("attempt_id") or ""),b(v.get("order_id") or ""),b(v.get("slot_id") or ""),b(v.get("campaign_id") or ""),b(v.get("creative_id") or ""),b(v.get("channel") or ""),b(v.get("provider") or ""),b(v.get("outcome") or ""),b(v.get("reason_code") or ""),int(v.get("value_minor") or 0),b(v.get("currency") or ""),b(v.get("query_text") or ""),int(v.get("result_count",-1)),b(metadata or ""))
    if rc:_err(c,rc)
def _commerce_event_safe(**v):
    try: commerce_event_record(**v)
    except Exception: pass
def commerce_funnel_dashboard(days=30): return _json_call(lib.fvs_commerce_funnel_dashboard,int(days))
def marketing_dashboard(days=30):
    result=_json_call(lib.fvs_marketing_dashboard,int(days))
    try: result["funnel"]=commerce_funnel_dashboard(days)
    except Exception: result["funnel"]={"unavailable":True}
    try: result["recovery"]=payment_recovery_dashboard(days)
    except Exception: result["recovery"]={"unavailable":True}
    try: result["search_quality"]=search_quality_dashboard(days)
    except Exception: result["search_quality"]={"unavailable":True}
    try: result["experiments"]=experiment_dashboard(days)
    except Exception: result["experiments"]={"unavailable":True}
    return result

def commerce_home(): return _json_call(lib.fvs_commerce_home)
def commerce_collection_get(slug): return _json_call(lib.fvs_commerce_collection_get,b(slug))
def commerce_recommendations(slot_id,limit=8): return _json_call(lib.fvs_commerce_recommendations,b(slot_id),int(limit))
def cart_apply_code(cart_id,secret,code): return _json_call(lib.fvs_cart_apply_code,b(cart_id),b(secret),b(code or ''))
def cart_addon_set(cart_id,secret,addon_code,quantity): return _json_call(lib.fvs_cart_addon_set,b(cart_id),b(secret),b(addon_code),int(quantity))
def cart_set_origin(cart_id,secret,channel='',campaign_id='',creative_id='',social_token=''):
    c=ctx();rc=lib.fvs_cart_set_origin(c,b(cart_id),b(secret),b(channel or ''),b(campaign_id or ''),b(creative_id or ''),b(social_token or ''))
    if rc:_err(c,rc)
def commerce_promotion_upsert(v,actor='commerce-admin'):
    c=ctx();rc=lib.fvs_commerce_promotion_upsert(c,b(v['code']),b(v['name']),b(v['discount_type']),int(v['discount_value']),int(v.get('min_subtotal_minor') or 0),int(v.get('max_discount_minor') or 0),b(v.get('currency') or 'MXN'),b(v.get('starts_at') or ''),b(v.get('ends_at') or ''),int(v.get('usage_limit') or 0),1 if v.get('active',True) else 0,b(__import__('json').dumps(v.get('channel_scope') or [],separators=(',',':'))),b(actor))
    if rc:_err(c,rc)
def commerce_collection_upsert(v,actor='commerce-admin'):
    c=ctx();dumps=lambda x: json.dumps(x or {},ensure_ascii=False,separators=(',',':'));rc=lib.fvs_commerce_collection_upsert(c,b(v['slug']),b(v['name']),b(v.get('subtitle') or ''),b(v.get('hero_image_url') or ''),b(v.get('badge') or ''),b(dumps(v.get('filter'))),b(dumps(v.get('merchandising'))),1 if v.get('active',True) else 0,int(v.get('sort_order') or 0),b(actor))
    if rc:_err(c,rc)
def social_catalog_feed(channel,limit=500,offset=0): return _json_call(lib.fvs_social_catalog_feed,b(channel),int(limit),int(offset))
def social_sales_link_create(v,actor='social-admin'): return _json_call(lib.fvs_social_sales_link_create,b(v['channel']),b(v.get('slot_id') or ''),b(v.get('collection_slug') or ''),b(v.get('campaign_id') or ''),b(v.get('creative_id') or ''),b(v.get('promo_code') or ''),b(actor))
def social_sales_link_resolve(token): return _json_call(lib.fvs_social_sales_link_resolve,b(token))
def social_lead_capture(v): return _json_call(lib.fvs_social_lead_capture,b(v['channel']),b(v['provider_lead_id']),b(json.dumps(v.get('contact') or {},ensure_ascii=False,separators=(',',':'))),b(v['message']),b(v.get('campaign_id') or ''),b(v.get('creative_id') or ''))

def seo_rebuild_inventory(actor='seo-admin'):
    c=ctx();n=ctypes.c_uint();rc=lib.fvs_seo_rebuild_inventory(c,b(actor),ctypes.byref(n))
    if rc:_err(c,rc)
    return int(n.value)
def seo_page_upsert(v,actor='seo-admin'):
    c=ctx();rc=lib.fvs_seo_page_upsert(c,b(v.get('inventory_slot_id') or ''),b(v['slug']),b(v.get('locale','es-MX')),b(v.get('page_type','property')),b(v['title']),b(v['meta_description']),b(v['h1']),b(v.get('body_text') or ''),b(json.dumps(v.get('faq') or [],ensure_ascii=False,separators=(',',':'))),b(v['canonical_path']),b(v.get('og_image_url') or ''),1 if v.get('indexable',True) else 0,b(actor))
    if rc:_err(c,rc)
def seo_page_get(slug,locale='es-MX'): return _json_call(lib.fvs_seo_page_get,b(slug),b(locale))
def seo_pages_list(limit=50000): return _json_call(lib.fvs_seo_pages_list,int(limit))
def seo_redirect_get(source_path): return _json_call(lib.fvs_seo_redirect_get,b(source_path))
