from __future__ import annotations
import os
import core
import payments

def _retry_seconds():
    try: value=int(os.getenv("FVS_PAYMENT_RETRY_SECONDS","30"))
    except ValueError: value=30
    return max(5,min(3600,value))

def provider_prepare(attempt,secret,public_base,api_base,public_attempt):
    aid=str(attempt.get("attempt_id") or "")
    try:
        if attempt["provider"]=="stripe":
            p=payments.stripe_create_or_resume(attempt)
        else:
            p=payments.mp_create_or_resume(attempt,api_base+"/api/v1/webhooks/mercadopago",public_base+"/?checkout=return")
        core.checkout_attach(aid,p.get("checkout_id"),p.get("payment_id"),p.get("client_secret"),p.get("redirect_url"))
        core.payment_recovery_mark(aid,"active","provider_ready","",0)
        return public_attempt(core.checkout_get(attempt["cart_id"],secret))
    except payments.ProviderError as exc:
        core.payment_recovery_mark(aid,"provider_error","provider_prepare",type(exc).__name__,_retry_seconds())
        raise
