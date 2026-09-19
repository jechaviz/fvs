(function () {
  'use strict';
  const VERSION = '6.10.0';
  const JS_URL = `https://unpkg.com/maplibre-gl@${VERSION}/dist/maplibre-gl.mjs`;
  const CSS_URL = `https://unpkg.com/maplibre-gl@${VERSION}/dist/maplibre-gl.css`;
  let promise = null;

  function ensureCss() {
    if (document.querySelector('link[data-fvs-maplibre]')) return;
    const link = document.createElement('link');
    link.rel = 'stylesheet';
    link.href = CSS_URL;
    link.dataset.fvsMaplibre = VERSION;
    document.head.appendChild(link);
  }

  window.FVS_LOAD_MAPLIBRE = function () {
    if (window.maplibregl) return Promise.resolve(window.maplibregl);
    if (promise) return promise;
    ensureCss();
    promise = import(JS_URL)
      .then((module) => {
        window.maplibregl = module;
        return module;
      })
      .catch((error) => {
        promise = null;
        console.warn('MapLibre load failed', error);
        throw error;
      });
    return promise;
  };
})();
