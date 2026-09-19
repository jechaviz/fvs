(function(){
  const amenityLabels={pool:'Alberca',wifi:'Wi‑Fi',beach:'Playa',parking:'Estacionamiento',kitchen:'Cocina',gym:'Gimnasio',spa:'Spa','ocean-view':'Vista al mar',ski:'Ski',fireplace:'Chimenea','pet-friendly':'Pet friendly'};
  const api=async(url,opt={})=>{const r=await fetch(url,{credentials:'same-origin',headers:{'Content-Type':'application/json',...(opt.headers||{})},...opt});const j=await r.json().catch(()=>({}));if(!r.ok){const e=new Error(j.error||'request_failed');e.status=r.status;throw e;}return j;};
  const pad=n=>String(n).padStart(2,'0');
  const debounce=(fn,ms)=>{let t;return(...a)=>{clearTimeout(t);t=setTimeout(()=>fn(...a),ms)}};
  const localJson=(key,fallback)=>{try{const v=JSON.parse(localStorage.getItem(key)||'null');return v??fallback}catch{return fallback}};
  const savedView=()=>{const v=localStorage.getItem('fvs_view');return ['list','split','map'].includes(v)?v:'split'};
  const methods={
    money(v,c){try{return new Intl.NumberFormat('es-MX',{style:'currency',currency:c||'MXN',maximumFractionDigits:0}).format((Number(v)||0)/100)}catch{return `${(Number(v)||0)/100} ${c}`}},
    shortMoney(v,c){const n=(Number(v)||0)/100;if(c==='MXN')return n>=1000?`$${Math.round(n/1000)}k`:`$${Math.round(n)}`;return new Intl.NumberFormat('en',{style:'currency',currency:c||'USD',notation:'compact',maximumFractionDigits:0}).format(n)},
    minorToMajor(v){return v?Math.round(Number(v)/100):''},
    fmtDate(s){if(!s)return '';return new Intl.DateTimeFormat('es-MX',{day:'numeric',month:'short',year:'numeric',timeZone:'UTC'}).format(new Date(s+'T00:00:00Z'))},
    fmtDateShort(s){if(!s)return '';return new Intl.DateTimeFormat('es-MX',{day:'numeric',month:'short',timeZone:'UTC'}).format(new Date(s+'T00:00:00Z'))},
    countdown(ts){if(!ts)return '—';const n=Math.max(0,Math.floor((new Date(ts).getTime()-this.now)/1000));return `${Math.floor(n/60)}:${String(n%60).padStart(2,'0')}`},
    seoUrl(p){return `/stay/vacation-rental-${String(p.id||'').slice(0,8)}`},
    amenityLabel(a){return amenityLabels[a]||String(a).replace(/[-_]/g,' ').replace(/^./,x=>x.toUpperCase())},
    suggestionGlyph(t){return t==='country'?'◎':t==='city'?'⌖':t==='resort'?'◇':'⌂'},
    suggestionTypeLabel(t){return t==='country'?'País':t==='city'?'Ciudad':t==='resort'?'Resort':'Propiedad'},
    socialChannelLabel(c){const m={instagram:'Instagram',meta:'Meta',facebook:'Facebook',tiktok:'TikTok',pinterest:'Pinterest',whatsapp:'WhatsApp',youtube:'YouTube',x:'X',linkedin:'LinkedIn',google:'Google'};return m[c]||c||'Social'}
  };
  const fallback='data:image/svg+xml;charset=UTF-8,'+encodeURIComponent('<svg xmlns="http://www.w3.org/2000/svg" width="900" height="600"><defs><linearGradient id="g" x1="0" x2="1"><stop stop-color="#edf2f7"/><stop offset="1" stop-color="#dfe7f1"/></linearGradient></defs><rect width="100%" height="100%" fill="url(#g)"/><text x="50%" y="50%" dominant-baseline="middle" text-anchor="middle" fill="#718096" font-family="sans-serif" font-size="32">FVS</text></svg>');
  window.FVS_UI=Object.freeze({api,pad,debounce,localJson,savedView,methods:Object.freeze(methods),fallback});
})();
