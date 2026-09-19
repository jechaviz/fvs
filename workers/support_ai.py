from __future__ import annotations
import hashlib, json, os, re, urllib.request
from pathlib import Path
import core

HANDOFF_RE=re.compile(r"\b(reembolso|refund|cancel(?:ar|ación)?|chargeback|fraude|fraud|emergencia|emergency|abogado|legal|demanda|dispute|cargo no reconocido|modificar reserva|cambiar reserva)\b",re.I)
PAN_RE=re.compile(r'(?<!\d)(?:\d[ -]?){13,19}(?!\d)')
CVV_RE=re.compile(r'(?i)\b(cvv|cvc|código de seguridad|codigo de seguridad)\s*[:=]?\s*\d{3,4}\b')

def _redact_sensitive(text:str)->str:
    text=CVV_RE.sub('[DATOS_DE_PAGO_REDACTADOS]',text)
    return PAN_RE.sub('[DATOS_DE_PAGO_REDACTADOS]',text)

def _knowledge():
    p=os.getenv('FVS_SUPPORT_KB_PATH','')
    if not p:return ''
    path=Path(p)
    try:return path.read_text(encoding='utf-8')[:120000]
    except Exception:return ''

def _extract_text(resp:dict)->str:
    if isinstance(resp.get('output_text'),str) and resp['output_text'].strip():return resp['output_text'].strip()
    parts=[]
    for item in resp.get('output') or []:
        for c in item.get('content') or []:
            if c.get('type') in ('output_text','text') and isinstance(c.get('text'),str):parts.append(c['text'])
    return '\n'.join(parts).strip()

def _call_openai(thread:dict)->tuple[str,str,int,int]:
    key=os.environ.get('OPENAI_API_KEY','')
    if not key:raise RuntimeError('OPENAI_API_KEY missing')
    model=os.getenv('FVS_SUPPORT_AI_MODEL','gpt-5.6-luna')
    messages=[]
    for m in (thread.get('messages') or [])[-24:]:
        if m.get('visibility')!='public':continue
        role='assistant' if m.get('sender_type')=='ai' else 'user'
        if m.get('sender_type')=='agent': role='assistant'
        messages.append({'role':role,'content':_redact_sensitive(str(m.get('body') or ''))[:8000]})
    system=("Eres el asistente de soporte de FVS, una plataforma de reservaciones vacacionales. "
            "Responde en el idioma del huésped, de forma breve, precisa y útil. No inventes disponibilidad, precios, políticas ni estados de pago. "
            "No ejecutes cancelaciones, reembolsos, modificaciones de reserva ni decisiones financieras. Para esas acciones, o si falta información crítica, "
            "responde exactamente con el prefijo [HANDOFF] seguido de una razón corta para que continúe un agente humano. "
            "Nunca solicites números completos de tarjeta, CVV, contraseñas o secretos. Si el huésped reporta fraude, cargo no reconocido, emergencia o disputa, escala.\n")
    kb=_knowledge()
    if kb:system+='\nBase de conocimiento aprobada:\n'+kb
    thread_key=str(thread.get('thread_id') or '')
    safety_id=hashlib.sha256(thread_key.encode('utf-8')).hexdigest() if thread_key else None
    body={'model':model,'store':False,'instructions':system,'input':messages,'max_output_tokens':700}
    if safety_id:
        body['safety_identifier']=safety_id
        body['prompt_cache_key']='support-'+safety_id[:48]
    req=urllib.request.Request('https://api.openai.com/v1/responses',data=json.dumps(body).encode(),headers={'Authorization':'Bearer '+key,'Content-Type':'application/json','User-Agent':'FVS-support/4.0'},method='POST')
    with urllib.request.urlopen(req,timeout=float(os.getenv('FVS_SUPPORT_AI_TIMEOUT','25'))) as r:resp=json.load(r)
    text=_extract_text(resp)
    if not text:raise RuntimeError('empty AI response')
    usage=resp.get('usage') or {}
    return text,model,int(usage.get('input_tokens') or 0),int(usage.get('output_tokens') or 0)

def process_event(evt:dict)->bool:
    if evt.get('event_type')!='support.ai_requested':return False
    thread_id=(evt.get('payload') or {}).get('thread_id') or evt.get('aggregate_id')
    if os.getenv('FVS_SUPPORT_AI_ENABLED','1')!='1':
        core.support_handoff(thread_id,'Asistencia IA deshabilitada; continuar con agente humano')
        return True
    thread=core.support_thread_context(thread_id)
    if not thread.get('ai_enabled'):return True
    public=[m for m in thread.get('messages') or [] if m.get('visibility')=='public']
    latest=str(public[-1].get('body') if public else '')
    if HANDOFF_RE.search(latest):
        core.support_handoff(thread_id,'Solicitud sensible o acción que requiere agente humano')
        return True
    text,model,inp,out=_call_openai(thread)
    if text.startswith('[HANDOFF]'):
        core.support_handoff(thread_id,text[len('[HANDOFF]'):].strip()[:500] or 'Escalamiento solicitado por IA')
        return True
    core.support_ai_message(thread_id,text,model,inp,out)
    return True
