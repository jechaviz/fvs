(function(){
  'use strict';
  if(window.FVS_MARKETING_ATTRIBUTION_ENABLED===false) return;
  const uuid=()=>crypto.randomUUID?crypto.randomUUID():([1e7]+-1e3+-4e3+-8e3+-1e11).replace(/[018]/g,c=>(c^crypto.getRandomValues(new Uint8Array(1))[0]&15>>c/4).toString(16));
  const safeStore=(store,key,factory)=>{try{let v=store.getItem(key);if(!v){v=factory();store.setItem(key,v)}return v}catch{return factory()}};
  const visitor=safeStore(localStorage,'fvs_visitor_id',uuid);
  const session=safeStore(sessionStorage,'fvs_session_id',uuid);
  const qs=new URLSearchParams(location.search);
  const campaign={
    utm_source:(qs.get('utm_source')||'').slice(0,120),utm_medium:(qs.get('utm_medium')||'').slice(0,120),
    utm_campaign:(qs.get('utm_campaign')||'').slice(0,160),utm_content:(qs.get('utm_content')||'').slice(0,160),
    channel:(qs.get('utm_source')||'').slice(0,32),referrer:(document.referrer||'').slice(0,1500),
    campaign_id:/^[0-9a-fA-F-]{36}$/.test(qs.get('campaign_id')||'')?qs.get('campaign_id'):'',
    creative_id:/^[0-9a-fA-F-]{36}$/.test(qs.get('creative_id')||'')?qs.get('creative_id'):''
  };
  async function track(event_type,extra={}){
    const body={visitor_id:visitor,session_id:session,event_type,...campaign,...extra};
    try{await fetch('/api/v1/marketing/event',{method:'POST',credentials:'same-origin',keepalive:true,headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})}catch(e){console.debug('FVS attribution unavailable',e)}
  }
  window.FVS_ATTR={visitor_id:visitor,session_id:session,track};
  track('landing');
})();
