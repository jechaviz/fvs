(function(){
  'use strict';
  const q=id=>document.getElementById(id);
  let current=null,poll=null,heartbeat=null;
  const headers=(json=false)=>({'X-Agent-Key':q('agentKey').value,'X-Agent-ID':q('agentId').value,...(json?{'Content-Type':'application/json'}:{})});
  async function api(path,opt={}){
    const r=await fetch(path,{credentials:'same-origin',...opt,headers:{...headers(!!opt.body),...(opt.headers||{})}});
    const j=await r.json().catch(()=>({}));
    if(!r.ok)throw new Error(j.error||`HTTP ${r.status}`);
    return j;
  }
  function el(tag,className,text){
    const node=document.createElement(tag);
    if(className)node.className=className;
    if(text!==undefined)node.textContent=String(text??'');
    return node;
  }
  async function beat(){
    if(!q('agentId').value||!q('agentKey').value)return;
    try{
      await api('/api/v1/agent/heartbeat',{method:'POST',body:JSON.stringify({display_name:q('agentName').value||q('agentId').value,status:'available',max_active:8})});
      q('presence').textContent='Disponible';
    }catch(e){q('presence').textContent='Error de presencia: '+e.message;}
  }
  async function loadQueue(){
    try{
      const j=await api('/api/v1/agent/queue');
      const nodes=(j.threads||[]).map(t=>{
        const b=el('button','queue-item');
        b.type='button';
        b.dataset.id=String(t.thread_id||'');
        b.append(el('strong','',t.subject||'Soporte'),el('span','',`${t.priority||'normal'} · ${t.status||''}`),el('small','',t.email||'sin email'));
        b.addEventListener('click',()=>openThread(b.dataset.id));
        return b;
      });
      q('queue').replaceChildren(...(nodes.length?nodes:[el('div','empty','Sin conversaciones pendientes.')]));
    }catch(e){q('queue').replaceChildren(el('div','empty','Error: '+e.message));}
  }
  async function openThread(id){
    try{current=await api('/api/v1/agent/threads/'+encodeURIComponent(id));renderThread();}
    catch(e){alert(e.message);}
  }
  function renderThread(){
    if(!current)return;
    q('threadEmpty').hidden=true;
    q('threadView').hidden=false;
    q('threadSubject').textContent=current.subject||'Soporte';
    q('threadMeta').textContent=`${current.status||''} · ${current.priority||''} · ${current.email||'sin email'}${current.assigned_agent_id?' · '+current.assigned_agent_id:''}`;
    const nodes=(current.messages||[]).map(m=>{
      const article=el('article','agent-message'+(m.visibility==='internal'?' is-internal':''));
      article.append(el('div','support-author',`${m.sender_type||'system'}${m.sender_id?' · '+m.sender_id:''}${m.visibility==='internal'?' · interna':''}`),el('div','',m.body||''));
      return article;
    });
    q('messages').replaceChildren(...nodes);
    q('messages').scrollTop=q('messages').scrollHeight;
  }
  q('connect').onclick=async()=>{await beat();await loadQueue();clearInterval(poll);clearInterval(heartbeat);poll=setInterval(loadQueue,5000);heartbeat=setInterval(beat,15000);};
  q('refresh').onclick=loadQueue;
  q('claim').onclick=async()=>{if(!current)return;try{current=await api(`/api/v1/agent/threads/${current.thread_id}/claim`,{method:'POST',body:'{}'});renderThread();await loadQueue();}catch(e){alert(e.message);}};
  q('resolve').onclick=async()=>{if(!current)return;try{current=await api(`/api/v1/agent/threads/${current.thread_id}/resolve`,{method:'POST',body:'{}'});renderThread();await loadQueue();}catch(e){alert(e.message);}};
  q('reply').onsubmit=async e=>{
    e.preventDefault();
    if(!current||!q('replyText').value.trim())return;
    try{
      current=await api(`/api/v1/agent/threads/${current.thread_id}/messages`,{method:'POST',body:JSON.stringify({body:q('replyText').value.trim(),internal:q('internal').checked})});
      q('replyText').value='';q('internal').checked=false;renderThread();await loadQueue();
    }catch(err){alert(err.message);}
  };
})();
