#!/usr/bin/env python3
from __future__ import annotations
import json, os, smtplib, socket, sys, time, secrets, signal
from email.message import EmailMessage
from email.utils import parseaddr
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"shim-python"))
import core
import support_ai

OUT=Path(os.getenv("FVS_VOUCHER_DIR",str(Path(__file__).resolve().parents[1]/"var/vouchers")))
OUT.mkdir(parents=True,exist_ok=True)
OWNER_PREFIX=f"py:{socket.gethostname()}:{os.getpid()}"
INSTANCE_ID=OWNER_PREFIX
STOP=False
def _stop(signum, frame):
    global STOP
    STOP=True
signal.signal(signal.SIGTERM,_stop); signal.signal(signal.SIGINT,_stop)

def pdf_escape(s): return str(s).replace('\\','\\\\').replace('(','\\(').replace(')','\\)')
def money_minor(v):
    n=int(v); sign='-' if n<0 else ''; n=abs(n); return f"{sign}{n//100}.{n%100:02d}"
def make_pdf(order):
    lines=["FVS · Confirmacion de reserva",f"Orden: {order['order_id']}",f"Cliente: {order['customer_email']}",f"Total: {money_minor(order['total_minor'])} {order['currency']}",""]
    for i,it in enumerate(order.get('items',[]),1):
        mode=(it.get('booking_mode') or 'fixed').lower()
        week=it.get('week_number')
        label=(f"Semana {week} · " if week else "")+("Flotante" if mode=='floating' else "Fija")
        if mode=='floating' and it.get('float_group'): label+=f" · Grupo {it['float_group']}"
        lines += [f"{i}. {it['resort']} · {it['unit_name']}",f"   {it['check_in']} a {it['check_out']} · {label}",f"   {it['guests']} huespedes · {money_minor(it['price_minor'])} {it['currency']}"]
    per_page=42
    chunks=[lines[i:i+per_page] for i in range(0,len(lines),per_page)] or [[]]
    n=len(chunks);font_obj=3+2*n
    objs=[b"<< /Type /Catalog /Pages 2 0 R >>"]
    kids=' '.join(f"{3+2*i} 0 R" for i in range(n))
    objs.append(f"<< /Type /Pages /Kids [{kids}] /Count {n} >>".encode())
    for page_no,chunk in enumerate(chunks,1):
        page_obj=3+2*(page_no-1);content_obj=page_obj+1
        content=["BT /F1 11 Tf 48 760 Td 14 TL"]
        for line in chunk: content.append(f"({pdf_escape(line)}) Tj T*")
        content.append(f"(Pagina {page_no}/{n}) Tj")
        content.append("ET")
        stream='\n'.join(content).encode('latin-1','replace')
        objs.append(f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 {font_obj} 0 R >> >> /Contents {content_obj} 0 R >>".encode())
        objs.append(b"<< /Length "+str(len(stream)).encode()+b" >>\nstream\n"+stream+b"\nendstream")
    objs.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    out=bytearray(b"%PDF-1.4\n");offs=[0]
    for i,obj in enumerate(objs,1):offs.append(len(out));out+=f"{i} 0 obj\n".encode()+obj+b"\nendobj\n"
    xref=len(out);out+=f"xref\n0 {len(objs)+1}\n0000000000 65535 f \n".encode()
    for off in offs[1:]:out+=f"{off:010d} 00000 n \n".encode()
    out+=f"trailer << /Size {len(objs)+1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode();return bytes(out)

def write_atomic(path,data):
    tmp=path.with_suffix(path.suffix+f".{os.getpid()}.tmp");tmp.write_bytes(data);os.replace(tmp,path)

def send_email(order,pdf_path):
    mode=os.getenv("FVS_EMAIL_MODE","log")
    if mode=="log": print(f"EMAIL(log) to={order['customer_email']} voucher={pdf_path}");return
    msg=EmailMessage();msg['Subject']=f"FVS · Reserva {order['order_id']}";msg['From']=os.environ['FVS_SMTP_FROM'];msg['To']=order['customer_email']
    cc=[]
    for raw in os.getenv('FVS_EMAIL_CC','').split(','):
        value=raw.strip()
        if not value:continue
        parsed=parseaddr(value)[1]
        if parsed!=value or '\r' in value or '\n' in value or '@' not in parsed:raise RuntimeError('invalid FVS_EMAIL_CC address')
        cc.append(parsed)
    if cc:msg['Cc']=', '.join(cc)
    domain=os.getenv('FVS_MESSAGE_ID_DOMAIN','fvs.local').replace('\r','').replace('\n','');msg['Message-ID']=f"<fvs-order-{order['order_id']}@{domain}>";msg['X-FVS-Order-ID']=order['order_id'];msg.set_content(f"Tu reserva FVS {order['order_id']} fue confirmada. Adjuntamos tu voucher.")
    msg.add_attachment(pdf_path.read_bytes(),maintype='application',subtype='pdf',filename=pdf_path.name)
    host=os.environ['FVS_SMTP_HOST'];port=int(os.getenv('FVS_SMTP_PORT','587'))
    with smtplib.SMTP(host,port,timeout=15) as s:
        s.ehlo();
        if os.getenv('FVS_SMTP_STARTTLS','1')=='1':s.starttls();s.ehlo()
        user=os.getenv('FVS_SMTP_USER');
        if user:s.login(user,os.getenv('FVS_SMTP_PASSWORD',''))
        s.send_message(msg)

def process(evt):
    if evt.get('event_type')=='support.ai_requested':
        if not support_ai.process_event(evt): raise RuntimeError('support event dispatch failed')
        return
    if evt.get('event_type')!='order.confirmed':
        raise RuntimeError('unsupported outbox event type')
    order_id=(evt.get('payload') or {}).get('order_id') or evt.get('aggregate_id');order=core.order_get(order_id)
    path=OUT/f"{order_id}.pdf";write_atomic(path,make_pdf(order));send_email(order,path)

def once():
    owner=OWNER_PREFIX+":"+secrets.token_hex(8)
    lease=max(30,min(3600,int(os.getenv('FVS_OUTBOX_LEASE_SECONDS','300'))));evt=core.outbox_claim(owner,lease)
    if not evt:return False
    eid=evt['event_id'];token=evt['lease_token']
    try:process(evt);core.outbox_ack(eid,owner,token)
    except Exception as e:
        print(f"worker error event={eid} type={type(e).__name__}",file=sys.stderr)
        try:core.outbox_nack(eid,owner,token,type(e).__name__)
        except Exception:pass
    return True

if __name__=='__main__':
    interval=float(os.getenv('FVS_WORKER_IDLE_SECONDS','2'));sweep_every=max(30,float(os.getenv('FVS_MAINTENANCE_INTERVAL_SECONDS','60')));heartbeat_every=max(5,float(os.getenv('FVS_WORKER_HEARTBEAT_SECONDS','15')));cart_ttl=int(os.getenv('FVS_OPEN_CART_TTL_SECONDS','86400'));next_sweep=0.0;next_heartbeat=0.0
    while not STOP:
        now=time.monotonic()
        if now>=next_heartbeat:
            try: core.worker_heartbeat('outbox',INSTANCE_ID)
            except Exception as e: print(f"heartbeat error type={type(e).__name__}",file=sys.stderr)
            next_heartbeat=now+heartbeat_every
        if now>=next_sweep:
            try:
                r=core.maintenance_sweep(cart_ttl)
                if r['expired_carts'] or r['released_holds']:print(f"MAINTENANCE expired_carts={r['expired_carts']} released_holds={r['released_holds']}")
            except Exception as e:print(f"maintenance error type={type(e).__name__}",file=sys.stderr)
            next_sweep=now+sweep_every
        if not once() and not STOP: time.sleep(min(interval,1.0))
    try:
        core.worker_goodbye('outbox',INSTANCE_ID)
    except Exception as e:
        print(f"goodbye heartbeat error type={type(e).__name__}",file=sys.stderr)
    print('worker shutdown complete')
