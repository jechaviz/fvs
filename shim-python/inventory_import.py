from __future__ import annotations
import csv, hashlib, io, re, zipfile
from datetime import date
from decimal import Decimal, ROUND_HALF_UP
from xml.etree import ElementTree as ET
import core

REQ={"resort","unit_code","unit_name","check_in","check_out"}

def _col(ref):
    m=re.match(r"([A-Z]+)",ref or "A");n=0
    for ch in (m.group(1) if m else "A"):n=n*26+ord(ch)-64
    return n-1

def _xlsx_rows(raw:bytes):
    with zipfile.ZipFile(io.BytesIO(raw)) as z:
        shared=[]
        if "xl/sharedStrings.xml" in z.namelist():
            root=ET.fromstring(z.read("xl/sharedStrings.xml"))
            for si in root:shared.append("".join(t.text or "" for t in si.iter() if t.tag.endswith("}t")))
        sheet=next((n for n in z.namelist() if n.startswith("xl/worksheets/sheet") and n.endswith(".xml")),None)
        if not sheet:raise ValueError("xlsx has no worksheet")
        root=ET.fromstring(z.read(sheet));rows=[]
        for r in (x for x in root.iter() if x.tag.endswith("}row")):
            vals={}
            for c in (x for x in r if x.tag.endswith("}c")):
                idx=_col(c.attrib.get("r","A"));typ=c.attrib.get("t");v=next((x for x in c if x.tag.endswith("}v")),None);inline=next((x for x in c.iter() if x.tag.endswith("}t")),None)
                text=inline.text if inline is not None else (v.text if v is not None else "")
                if typ=="s" and text: text=shared[int(text)]
                vals[idx]=text or ""
            if vals:rows.append([vals.get(i,"") for i in range(max(vals)+1)])
        return rows

def rows_from_bytes(raw:bytes,filename:str):
    ext=filename.lower().rsplit('.',1)[-1] if '.' in filename else 'csv'
    if ext in {'xlsx','xlsm'}: rows=_xlsx_rows(raw)
    else:
        text=raw.decode('utf-8-sig');dialect=csv.Sniffer().sniff(text[:4096],delimiters=',;\t') if text.strip() else csv.excel
        rows=list(csv.reader(io.StringIO(text),dialect))
    if not rows:return []
    headers=[re.sub(r'[^a-z0-9]+','_',str(x).strip().lower()).strip('_') for x in rows[0]]
    return [dict(zip(headers,r)) for r in rows[1:] if any(str(x).strip() for x in r)]

def _minor(row):
    if str(row.get('price_minor','')).strip():return int(str(row['price_minor']).replace(',','').strip())
    raw=str(row.get('price','')).replace('$','').replace(',','').strip()
    return int((Decimal(raw)*100).quantize(Decimal('1'),rounding=ROUND_HALF_UP))

def normalize(row):
    missing=[k for k in REQ if not str(row.get(k,'')).strip()]
    if missing:raise ValueError('missing '+','.join(sorted(missing)))
    src=str(row.get('source_ref','')).strip()
    if not src:
        basis='|'.join(str(row.get(k,'')) for k in ('resort','unit_code','check_in','check_out'))
        src='auto:'+hashlib.sha256(basis.encode()).hexdigest()[:40]
    active=str(row.get('active','1')).strip().lower() not in {'0','false','no','inactive'}
    ci=str(row['check_in']).strip()[:10]
    mode=str(row.get('booking_mode') or 'fixed').strip().lower()
    if mode not in {'fixed','floating'}: raise ValueError('invalid booking_mode')
    fg=str(row.get('float_group','')).strip()
    if mode=='floating' and not fg: raise ValueError('floating inventory requires float_group')
    week_raw=str(row.get('week_number','')).strip()
    week=int(week_raw) if week_raw else date.fromisoformat(ci).isocalendar().week
    amenities=[x.strip() for x in re.split(r'[,;|]',str(row.get('amenities',''))) if x.strip()]
    return {'source_ref':src,'resort':str(row['resort']).strip(),'unit_code':str(row['unit_code']).strip(),'unit_name':str(row['unit_name']).strip(),'city':str(row.get('city','')).strip(),'country':str(row.get('country','')).strip(),'check_in':ci,'check_out':str(row['check_out']).strip()[:10],'week_number':week,'booking_mode':mode,'float_group':fg,'max_guests':int(row.get('max_guests') or 2),'price_minor':_minor(row),'currency':str(row.get('currency') or 'MXN').strip().upper(),'image_url':str(row.get('image_url','')).strip(),'short_description':str(row.get('short_description','')).strip(),'season':str(row.get('season','')).strip(),'latitude':str(row.get('latitude','')).strip(),'longitude':str(row.get('longitude','')).strip(),'property_type':str(row.get('property_type','')).strip(),'bedrooms':int(row.get('bedrooms') or 0),'bathrooms':float(row.get('bathrooms') or 0),'rating':float(row.get('rating') or 0),'amenities':amenities,'active':active}

def import_bytes(raw:bytes,filename:str):
    rows=rows_from_bytes(raw,filename);report={'rows_seen':len(rows),'inserted':0,'updated':0,'rejected':0,'errors':[]}
    for i,row in enumerate(rows,2):
        try:
            created=core.inventory_upsert(normalize(row));report['inserted' if created else 'updated']+=1
        except Exception as e:
            report['rejected']+=1
            if len(report['errors'])<50:report['errors'].append({'row':i,'error':str(e)[:240]})
    return report
