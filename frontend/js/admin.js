(function(){
  'use strict';
  const q=id=>document.getElementById(id),key=()=>q('key').value,actor=()=>q('actor').value||'backoffice';
  let campaigns=[];
  async function api(path,opts={}){
    const headers={...(opts.headers||{}),'X-Admin-Key':key(),'X-Admin-Actor':actor()};
    if(opts.body&&!(opts.body instanceof Blob)&&!headers['Content-Type'])headers['Content-Type']='application/json';
    const r=await fetch(path,{...opts,headers,credentials:'same-origin'});
    const ct=r.headers.get('content-type')||'';
    const body=ct.includes('json')?await r.json():await r.text();
    if(!r.ok)throw new Error((body&&body.error)||`HTTP ${r.status}`);
    return body;
  }
  const json=id=>{try{return JSON.parse(q(id).value||'{}');}catch{throw new Error(`JSON inválido en ${id}`);}};
  const selected=()=>q('campaignSelect').value;
  function el(tag,className,text){const n=document.createElement(tag);if(className)n.className=className;if(text!==undefined)n.textContent=String(text??'');return n;}
  function renderKpis(d){
    const f=d.funnel||{},pct=v=>(Number(v||0)*100).toFixed(1)+'%';
    const data=[
      ['Impresiones',d.impressions],['Clicks',d.clicks],['Leads',d.leads],['Reservas paid',f.bookings??d.bookings],
      ['ROAS',Number(d.roas||0).toFixed(2)+'×'],['Spend',d.spend_minor],
      ['Búsquedas',f.searches],['Zero results',f.zero_results],['Search → cart',pct(f.search_to_cart)],
      ['Carritos',f.carts],['Checkouts',f.checkouts],['Checkout → booking',pct(f.checkout_to_booking)],
      ['Fallos pago',f.payment_failures],
      ['Provider errors',d.recovery?.provider_error],['Revisión manual',d.recovery?.manual_review],['Recovery stale',d.recovery?.stale_recoverable],
      ['Search zero-rate',pct(d.search_quality?.zero_result_rate)],['MRR',Number(d.search_quality?.last_eval?.mrr||0).toFixed(3)],['NDCG@10',Number(d.search_quality?.last_eval?.ndcg10||0).toFixed(3)],
      ['Experiment evidence',Array.isArray(d.experiments?.evidence)?d.experiments.evidence.length:0],
      ['Abandonment queued',d.abandonment?.scheduled],['Abandonment converted',d.abandonment?.converted]
    ];
    q('mktKpis').replaceChildren(...data.map(([label,value])=>{const box=el('div');box.append(el('small','',label),el('strong','',value??0));return box;}));
  }
  async function refreshGrowth(){
    const [list,d]=await Promise.all([api('/api/v1/admin/marketing/campaigns'),api('/api/v1/admin/marketing/dashboard?days=30')]);
    campaigns=list.campaigns||[];
    const old=selected();
    const opts=campaigns.map(c=>{const o=document.createElement('option');o.value=c.id;o.textContent=`${c.name} · ${c.status} · ${c.objective}`;return o;});
    q('campaignSelect').replaceChildren(...opts);
    if(campaigns.some(c=>c.id===old))q('campaignSelect').value=old;
    renderCampaign();renderKpis(d);
  }
  function renderCampaign(){const c=campaigns.find(x=>x.id===selected());q('campaignDetail').textContent=c?JSON.stringify(c,null,2):'Sin campaña seleccionada';}
  async function guarded(fn){try{await fn();}catch(e){alert(e.message);}}
  q('campaignSelect').onchange=renderCampaign;
  q('mktRefresh').onclick=()=>guarded(refreshGrowth);
  q('campaignForm').onsubmit=e=>guarded(async()=>{e.preventDefault();await api('/api/v1/admin/marketing/campaigns',{method:'POST',body:JSON.stringify({name:q('campaignName').value,objective:q('campaignObjective').value,automation_mode:q('campaignMode').value,budget_minor:+q('campaignBudget').value,daily_budget_minor:+q('campaignDaily').value,currency:'MXN',utm_campaign:q('campaignUtm').value})});await refreshGrowth();});
  q('campaignApprove').onclick=()=>guarded(async()=>{if(!selected())return;await api(`/api/v1/admin/marketing/campaigns/${selected()}/approve`,{method:'POST',body:'{}'});await refreshGrowth();});
  q('campaignPause').onclick=()=>guarded(async()=>{if(!selected())return;await api(`/api/v1/admin/marketing/campaigns/${selected()}/pause`,{method:'POST',body:'{}'});await refreshGrowth();});
  q('guardrailForm').onsubmit=e=>guarded(async()=>{e.preventDefault();if(!selected())throw new Error('Selecciona campaña');await api(`/api/v1/admin/marketing/campaigns/${selected()}/configure`,{method:'POST',body:JSON.stringify({max_daily_spend_minor:+q('maxDaily').value,frequency_cap_7d:+q('freqCap').value,target_roas_bps:+q('targetRoas').value,stop_loss_minor:+q('stopLoss').value,audience:json('audienceJson'),geo:json('geoJson'),placements:json('placementsJson'),optimization_rules:json('rulesJson'),experiment:json('experimentJson')})});await refreshGrowth();});
  q('creativeForm').onsubmit=e=>guarded(async()=>{e.preventDefault();if(!selected())return;const r=await api('/api/v1/admin/marketing/creatives',{method:'POST',body:JSON.stringify({campaign_id:selected(),channel:q('creativeChannel').value,variant_key:q('creativeVariant').value,headline:q('creativeHeadline').value,body:q('creativeBody').value,cta:q('creativeCta').value,landing_url:new URL(q('creativeLanding').value,location.origin).href,image_url:q('creativeImage').value,ai_generated:q('creativeAi').checked})});q('creativeResult').textContent=JSON.stringify(r,null,2);if(r.creative_id)q('jobCreative').value=r.creative_id;});
  q('jobForm').onsubmit=e=>guarded(async()=>{e.preventDefault();if(!selected())return;const raw=q('jobAt').value;const at=raw.replace('T',' ')+(raw.length===16?':00':'');const r=await api('/api/v1/admin/marketing/jobs',{method:'POST',body:JSON.stringify({campaign_id:selected(),creative_id:q('jobCreative').value,channel:q('jobChannel').value,action:q('jobAction').value,scheduled_at:at,payload:json('jobPayload')})});q('jobResult').textContent=`Job ${r.job_id} · ${r.status}`;});
  q('promoForm').onsubmit=e=>guarded(async()=>{e.preventDefault();await api('/api/v1/admin/commerce/promotions',{method:'POST',body:JSON.stringify({code:q('promoCode').value.trim().toUpperCase(),name:q('promoName').value,discount_type:q('promoType').value,discount_value:+q('promoValue').value,min_subtotal_minor:+q('promoMin').value,max_discount_minor:+q('promoMax').value,currency:'MXN',usage_limit:+q('promoUsage').value,channel_scope:(q('promoChannels')?.value||'').split(',').map(x=>x.trim()).filter(Boolean),active:true})});q('promoResult').textContent='Promoción guardada';});
  q('collectionForm').onsubmit=e=>guarded(async()=>{e.preventDefault();await api('/api/v1/admin/commerce/collections',{method:'POST',body:JSON.stringify({slug:q('collectionSlug').value.trim().toLowerCase(),name:q('collectionName').value,subtitle:q('collectionSubtitle').value,filter:json('collectionFilter'),merchandising:json('collectionMerch'),active:true,sort_order:10})});q('collectionResult').textContent='Colección guardada';});
  q('socialLinkForm').onsubmit=e=>guarded(async()=>{e.preventDefault();const r=await api('/api/v1/admin/social/links',{method:'POST',body:JSON.stringify({channel:q('socialChannel').value,slot_id:q('socialSlot').value.trim(),collection_slug:q('socialCollection').value.trim(),campaign_id:q('socialCampaign').value.trim(),creative_id:q('socialCreative').value.trim(),promo_code:q('socialPromo').value.trim().toUpperCase()})});const url=new URL(r.path,location.origin).href;q('socialLinkResult').textContent=`${url}\nToken: ${r.token}`;});
  q('seoRebuild').onclick=()=>guarded(async()=>{const r=await api('/api/v1/admin/seo/rebuild',{method:'POST',body:'{}'});q('seoResult').textContent=`${r.upserted} filas procesadas`;});
  q('seoPageForm').onsubmit=e=>guarded(async()=>{e.preventDefault();const slug=q('seoSlug').value.toLowerCase().trim();const r={slug,locale:'es-MX',page_type:q('seoType').value,title:q('seoTitle').value,meta_description:q('seoMeta').value,h1:q('seoH1').value,body_text:q('seoBody').value,faq:json('seoFaq'),canonical_path:'/stay/'+slug,indexable:true};await api('/api/v1/admin/seo/pages',{method:'POST',body:JSON.stringify(r)});alert('Página SEO publicada');});
  q('ops').onclick=async()=>{q('opsResult').textContent='Consultando…';try{q('opsResult').textContent=JSON.stringify(await api('/api/v1/admin/ops'),null,2);}catch(e){q('opsResult').textContent='Error: '+e.message;}};
  q('review').onclick=async()=>{q('opsResult').textContent='Consultando revisión manual…';try{q('opsResult').textContent=JSON.stringify(await api('/api/v1/admin/manual-review?limit=100'),null,2);}catch(e){q('opsResult').textContent='Error: '+e.message;}};
  q('upload').onclick=async()=>{const f=q('file').files[0];if(!f||!key()){q('result').textContent='Selecciona archivo e ingresa la clave.';return;}q('upload').disabled=true;q('result').textContent='Importando…';try{const j=await api('/api/v1/admin/import',{method:'POST',headers:{'X-Filename':f.name,'Content-Type':'application/octet-stream'},body:f});q('result').textContent=JSON.stringify(j,null,2);}catch(e){q('result').textContent='Error: '+e.message;}finally{q('upload').disabled=false;}};
  q('requeue').onclick=async()=>{const id=q('eventId').value.trim();if(!/^[0-9a-fA-F-]{36}$/.test(id)){q('requeueResult').textContent='UUID inválido.';return;}q('requeueResult').textContent='Reencolando…';try{q('requeueResult').textContent=JSON.stringify(await api('/api/v1/admin/outbox/'+encodeURIComponent(id)+'/requeue',{method:'POST',body:'{}'}),null,2);}catch(e){q('requeueResult').textContent='Error: '+e.message;}};
})();
