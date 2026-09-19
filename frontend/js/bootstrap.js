(function(){
  const appEl=document.getElementById('app');
  const fail=(msg)=>{appEl.removeAttribute('un-cloak');const box=document.createElement('div');box.style.cssText='padding:40px;font-family:system-ui';const h=document.createElement('h2');h.textContent='No se pudo iniciar FVS';const p=document.createElement('p');p.textContent=String(msg);box.append(h,p);appEl.replaceChildren(box);};
  window.addEventListener('error',e=>{ if(appEl.hasAttribute('un-cloak')) fail('No fue posible cargar una dependencia del frontend.'); });
  window.addEventListener('DOMContentLoaded',async()=>{
    try{
      if(!window.Vue||!window['vue3-sfc-loader']) throw new Error('CDN runtime missing');
      const {loadModule}=window['vue3-sfc-loader'];
      const options={moduleCache:{vue:window.Vue},async getFile(url){const r=await fetch(url,{cache:'no-cache'});if(!r.ok)throw new Error('SFC '+r.status);return {getContentData:asBinary=>asBinary?r.arrayBuffer():r.text()};},addStyle(text){const s=document.createElement('style');s.textContent=text;document.head.appendChild(s);}};
      const Root={components:{App:window.Vue.defineAsyncComponent(()=>loadModule('/components/App.vue',options))},template:'<App />'};
      const app=window.Vue.createApp(Root);app.config.errorHandler=(err)=>{console.error(err);fail('Ocurrió un error al iniciar la interfaz.');};app.mount('#app');appEl.removeAttribute('un-cloak');
    }catch(e){console.error(e);fail('No fue posible cargar Vue/SFC.');}
  });
})();
