from __future__ import annotations
import base64, hashlib, hmac, json, os, time, urllib.error, urllib.parse, urllib.request
from decimal import Decimal, ROUND_HALF_UP

class ProviderError(RuntimeError): pass

class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None

_OPENER=urllib.request.build_opener(_NoRedirect)

def _request(url, *, method="GET", headers=None, data=None, timeout=12):
    if not url.startswith("https://"):
        raise ProviderError("provider URL must use HTTPS")
    req=urllib.request.Request(url, data=data, headers=headers or {}, method=method)
    try:
        with _OPENER.open(req, timeout=timeout) as r:
            raw=r.read(1024*1024)
            return json.loads(raw.decode("utf-8"), parse_float=Decimal)
    except urllib.error.HTTPError as e:
        detail=e.read(4096).decode("utf-8","replace")
        raise ProviderError(f"provider HTTP {e.code}: {detail[:512]}") from e
    except Exception as e:
        raise ProviderError(f"provider transport failure: {type(e).__name__}") from e

def _minor_to_decimal(amount_minor:int)->str:
    return format((Decimal(amount_minor)/Decimal(100)).quantize(Decimal("0.01")),"f")

def decimal_to_minor(value)->int:
    d=Decimal(str(value)).quantize(Decimal("0.01"), rounding=ROUND_HALF_UP)
    return int(d*100)

def stripe_create_or_resume(attempt:dict)->dict:
    key=os.environ["STRIPE_SECRET_KEY"]
    existing=attempt.get("provider_checkout_id")
    headers={"Authorization":"Basic "+base64.b64encode((key+":").encode()).decode(),"User-Agent":"FVS/6.0.0"}
    if existing:
        pi=_request("https://api.stripe.com/v1/payment_intents/"+urllib.parse.quote(existing),headers=headers)
    else:
        body=urllib.parse.urlencode({
            "amount":str(attempt["amount_minor"]),"currency":attempt["currency"].lower(),
            "receipt_email":attempt["email"],"automatic_payment_methods[enabled]":"true",
            "metadata[attempt_id]":attempt["attempt_id"],"metadata[cart_version]":str(attempt["cart_version"]),
        }).encode()
        headers={**headers,"Content-Type":"application/x-www-form-urlencoded","Idempotency-Key":attempt["idempotency_key"]}
        pi=_request("https://api.stripe.com/v1/payment_intents",method="POST",headers=headers,data=body)
    md=pi.get("metadata") or {}
    if (str(pi.get("object") or "")!="payment_intent" or
        (existing and str(pi.get("id") or "")!=str(existing)) or
        int(pi.get("amount",-1))!=int(attempt["amount_minor"]) or
        str(pi.get("currency","")).upper()!=attempt["currency"].upper() or
        md.get("attempt_id")!=attempt["attempt_id"] or
        str(md.get("cart_version"))!=str(attempt["cart_version"]) or
        str(pi.get("status") or "") in {"canceled"}):
        raise ProviderError("Stripe object does not match FVS attempt")
    return {"checkout_id":pi["id"],"payment_id":None,"client_secret":pi.get("client_secret"),"redirect_url":None}

def stripe_verify_signature(raw:bytes, header:str, secret:str, tolerance=300):
    if not secret: raise ProviderError("Stripe webhook secret is not configured")
    parts={}
    for item in (header or "").split(","):
        if "=" in item:
            k,v=item.split("=",1); parts.setdefault(k.strip(),[]).append(v.strip())
    try: ts=int(parts.get("t",[""])[0])
    except ValueError: raise ProviderError("invalid Stripe signature timestamp")
    if abs(int(time.time())-ts)>tolerance: raise ProviderError("stale Stripe signature")
    expected=hmac.new(secret.encode(),str(ts).encode()+b"."+raw,hashlib.sha256).hexdigest()
    if not any(hmac.compare_digest(expected,v) for v in parts.get("v1",[])): raise ProviderError("invalid Stripe signature")

def stripe_fetch_payment(payment_id:str)->dict:
    key=os.environ["STRIPE_SECRET_KEY"]
    headers={"Authorization":"Basic "+base64.b64encode((key+":").encode()).decode(),"User-Agent":"FVS/6.0.0"}
    return _request("https://api.stripe.com/v1/payment_intents/"+urllib.parse.quote(payment_id),headers=headers)

def mp_create_or_resume(attempt:dict, notification_url:str, return_url:str)->dict:
    token=os.environ["MERCADOPAGO_ACCESS_TOKEN"]
    headers={"Authorization":"Bearer "+token,"Content-Type":"application/json","X-Idempotency-Key":attempt["idempotency_key"],"User-Agent":"FVS/6.0.0"}
    existing=attempt.get("provider_checkout_id")
    if existing:
        pref=_request("https://api.mercadopago.com/checkout/preferences/"+urllib.parse.quote(existing),headers=headers)
    else:
        payload={
            "items":[{"id":attempt["attempt_id"],"title":"Reserva FVS","quantity":1,"currency_id":attempt["currency"].upper(),"unit_price":_minor_to_decimal(int(attempt["amount_minor"]))}],
            "payer":{"email":attempt["email"]},"external_reference":attempt["attempt_id"],
            "metadata":{"attempt_id":attempt["attempt_id"],"cart_version":str(attempt["cart_version"])},
            "notification_url":notification_url,"back_urls":{"success":return_url,"pending":return_url,"failure":return_url},"auto_return":"approved"
        }
        pref=_request("https://api.mercadopago.com/checkout/preferences",method="POST",headers=headers,data=json.dumps(payload).encode())
    items=pref.get("items") or []
    item=items[0] if len(items)==1 else {}
    amount=decimal_to_minor(item.get("unit_price",0)) if item else -1
    currency=str(item.get("currency_id","")).upper() if item else ""
    md=pref.get("metadata") or {}
    if ((existing and str(pref.get("id") or "")!=str(existing)) or
        pref.get("external_reference")!=attempt["attempt_id"] or
        str(item.get("id") or "")!=attempt["attempt_id"] or
        int(item.get("quantity") or 0)!=1 or
        amount!=int(attempt["amount_minor"]) or
        currency!=attempt["currency"].upper() or
        str(md.get("attempt_id") or "")!=attempt["attempt_id"] or
        str(md.get("cart_version") or "")!=str(attempt["cart_version"])):
        raise ProviderError("Mercado Pago preference does not match FVS attempt")
    redirect=pref.get("init_point") or pref.get("sandbox_init_point")
    if not isinstance(redirect,str) or not redirect.startswith("https://"):
        raise ProviderError("Mercado Pago preference has invalid redirect URL")
    return {"checkout_id":pref["id"],"payment_id":None,"client_secret":None,"redirect_url":redirect}

def mp_verify_signature(signature:str, request_id:str, data_id:str, secret:str):
    if not secret: raise ProviderError("Mercado Pago webhook secret is not configured")
    bits={}
    for part in (signature or "").split(","):
        if "=" in part:
            k,v=part.split("=",1);bits[k.strip()]=v.strip()
    ts=bits.get("ts"); supplied=bits.get("v1")
    if not ts or not supplied: raise ProviderError("invalid Mercado Pago signature")
    manifest=""
    if data_id: manifest+=f"id:{data_id};"
    if request_id: manifest+=f"request-id:{request_id};"
    if ts: manifest+=f"ts:{ts};"
    expected=hmac.new(secret.encode(),manifest.encode(),hashlib.sha256).hexdigest()
    if not hmac.compare_digest(expected,supplied): raise ProviderError("invalid Mercado Pago signature")

def mp_fetch_payment(payment_id:str)->dict:
    token=os.environ["MERCADOPAGO_ACCESS_TOKEN"]
    return _request("https://api.mercadopago.com/v1/payments/"+urllib.parse.quote(payment_id),headers={"Authorization":"Bearer "+token,"User-Agent":"FVS/6.0.0"})
