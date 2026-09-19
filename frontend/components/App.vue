<template>
  <main class="discovery-shell" :class="{'is-map-only':view==='map','is-list-only':view==='list'}">
    <header class="topbar discovery-topbar">
      <a class="brand brand-link" href="/" aria-label="FVS inicio">FVS<small>Vacation Store</small></a>
      <nav class="top-actions" aria-label="Acciones rápidas">
        <button class="ghost-icon desktop-only" @click="focusSearch" title="Buscar · /"><span aria-hidden="true">⌕</span><span>Buscar</span><kbd>/</kbd></button>
        <button class="ghost-icon desktop-only" @click="toggleView" title="Alternar mapa · M"><span aria-hidden="true">⌘</span><span>{{ view==='map'?'Lista':'Mapa' }}</span><kbd>M</kbd></button>
        <button class="cart-button" :class="{'has-items':cartCount>0}" @click="drawer=true" aria-controls="reservation-drawer" :aria-expanded="drawer ? 'true' : 'false'">
          <span class="cart-dot" v-if="cartCount">{{ cartCount }}</span><span>Reservas</span><span class="cart-total" v-if="cart?.total_minor">{{ money(cart.total_minor,cart.currency) }}</span>
        </button>
      </nav>
    </header>

    <section class="hero-search" aria-label="Buscar propiedades">
      <div class="omni-wrap" :class="{'is-open':suggestionsOpen}">
        <div class="omni-icon" aria-hidden="true">⌕</div>
        <div class="omni-field">
          <label for="destination-search">Destino, ciudad, país o propiedad</label>
          <input id="destination-search" ref="omni" v-model.trim="search.q" @input="onQueryInput" @focus="openSuggestions" @keydown.down.prevent="moveSuggestion(1)" @keydown.up.prevent="moveSuggestion(-1)" @keydown.enter.prevent="acceptHighlighted" @keydown.esc="suggestionsOpen=false" autocomplete="off" placeholder="Ej. Cancún, México, Ocean View…">
        </div>
        <button v-if="search.q" class="clear-mini" @click="clearQuery" aria-label="Limpiar búsqueda">×</button>
        <Transition name="fade-slide">
          <div v-if="suggestionsOpen" class="suggestions" role="listbox" aria-label="Sugerencias">
            <div class="suggestion-caption" v-if="suggesting">Buscando coincidencias…</div>
            <button v-for="(s,i) in suggestions" :key="s.type+'-'+s.label+'-'+i" class="suggestion-row" :class="{'is-active':i===suggestionIndex}" @mousedown.prevent="selectSuggestion(s)" role="option" :aria-selected="i===suggestionIndex">
              <span class="suggestion-type" :data-type="s.type">{{ suggestionGlyph(s.type) }}</span>
              <span><strong>{{ s.label }}</strong><small>{{ suggestionTypeLabel(s.type) }}<template v-if="s.subtitle"> · {{ s.subtitle }}</template></small></span>
              <span class="suggestion-arrow" aria-hidden="true">↗</span>
            </button>
            <button v-if="search.q && !suggestions.length && !suggesting" class="suggestion-row" @mousedown.prevent="searchFreeText"><span class="suggestion-type">⌕</span><span><strong>Buscar “{{ search.q }}”</strong><small>Búsqueda libre en propiedades y descripciones</small></span><span>↵</span></button>
            <div class="suggestion-hints"><span><kbd>↑</kbd><kbd>↓</kbd> navegar</span><span><kbd>↵</kbd> elegir</span><span><kbd>esc</kbd> cerrar</span></div>
          </div>
        </Transition>
      </div>

      <div class="search-segment date-segment">
        <label>Entrada</label><input type="date" v-model="search.check_in" @change="scheduleSearch(0)">
      </div>
      <div class="search-segment date-segment">
        <label>Salida</label><input type="date" v-model="search.check_out" @change="scheduleSearch(0)">
      </div>
      <div class="search-segment guest-segment">
        <label>Huéspedes</label><div class="guest-stepper"><button @click="stepGuests(-1)" aria-label="Quitar huésped">−</button><strong>{{ search.guests }}</strong><button @click="stepGuests(1)" aria-label="Agregar huésped">+</button></div>
      </div>
      <button class="search-cta" @click="loadInventory" :disabled="busy"><span v-if="!busy">Buscar</span><span v-else class="spinner-label"><i></i> Buscando</span></button>
    </section>

    <Transition name="fade-slide">
      <section v-if="socialOrigin.channel || socialOrigin.slot || socialOrigin.collection" class="social-offer-banner" aria-label="Oferta social">
        <div class="social-offer-icon" aria-hidden="true">↗</div>
        <div><span class="eyebrow">Oferta conectada · {{ socialChannelLabel(socialOrigin.channel) }}</span><strong>{{ socialOrigin.collection ? 'Colección seleccionada para ti' : 'Llegaste directo a una estancia disponible' }}</strong><small v-if="socialOrigin.promo">Código {{ socialOrigin.promo }} listo para aplicar en tu carrito.</small></div>
        <button v-if="socialOrigin.slot" class="primary compact" @click="reserveSocialSlot">Ver y reservar <span>→</span></button>
        <button v-else-if="socialOrigin.collection" class="primary compact" @click="openCollectionBySlug(socialOrigin.collection)">Explorar colección <span>→</span></button>
      </section>
    </Transition>

    <section v-if="commerce.collections.length" class="commerce-shelf" aria-label="Colecciones destacadas">
      <header class="commerce-shelf-head"><div><span class="eyebrow">Curado para convertir intención en viaje</span><h2>Colecciones FVS</h2></div><span class="shelf-counter">{{ commerce.collections.length }} escenarios</span></header>
      <div class="collection-rail">
        <button v-for="c in commerce.collections" :key="c.id" class="collection-card" :class="{'is-active':activeCollection?.slug===c.slug}" @click="openCollection(c)">
          <img v-if="c.hero_image_url" :src="c.hero_image_url" :alt="c.name" loading="lazy"><span v-else class="collection-fallback">◇</span>
          <span class="collection-gradient"></span><span class="collection-copy"><em v-if="c.badge">{{ c.badge }}</em><strong>{{ c.name }}</strong><small>{{ c.subtitle || 'Selección dinámica según disponibilidad' }}</small></span><span class="collection-arrow">↗</span>
        </button>
      </div>
    </section>

    <section class="filter-ribbon" aria-label="Filtros rápidos">
      <button class="filter-master" :class="{'is-active':advancedFilterCount}" @click="showFilters=!showFilters"><span aria-hidden="true">☷</span> Filtros <b v-if="advancedFilterCount">{{ advancedFilterCount }}</b></button>
      <div class="quick-filters">
        <button v-for="f in topCountries" :key="'country-'+f.value" class="filter-chip" :class="{'is-active':search.country===f.value}" @click="toggleExact('country',f.value)">{{ f.value }} <small>{{ f.count }}</small></button>
        <button v-for="f in topCities" :key="'city-'+f.value" class="filter-chip" :class="{'is-active':search.city===f.value}" @click="toggleExact('city',f.value)">{{ f.value }} <small>{{ f.count }}</small></button>
        <button v-for="f in topTypes" :key="'type-'+f.value" class="filter-chip" :class="{'is-active':search.property_type===f.value}" @click="toggleExact('property_type',f.value)">{{ f.value }} <small>{{ f.count }}</small></button>
      </div>
      <button v-if="activeFilterCount" class="clear-filters" @click="clearAdvancedFilters">Limpiar {{ activeFilterCount }}</button>
    </section>

    <Transition name="filter-panel">
      <section v-if="showFilters" class="advanced-filters" aria-label="Filtros avanzados">
        <header><div><span class="eyebrow">Refina sin salir del flujo</span><h2>Filtros avanzados</h2></div><button class="close" @click="showFilters=false" aria-label="Cerrar filtros">×</button></header>
        <div class="filter-grid">
          <div class="filter-block"><label>País</label><select v-model="search.country" @change="onHierarchyChange('country')"><option value="">Todos</option><option v-for="f in facets.countries||[]" :key="f.value" :value="f.value">{{ f.value }} · {{ f.count }}</option></select></div>
          <div class="filter-block"><label>Ciudad</label><select v-model="search.city" @change="scheduleSearch(0)"><option value="">Todas</option><option v-for="f in facets.cities||[]" :key="f.value" :value="f.value">{{ f.value }} · {{ f.count }}</option></select></div>
          <div class="filter-block"><label>Tipo de propiedad</label><select v-model="search.property_type" @change="scheduleSearch(0)"><option value="">Todos</option><option v-for="f in facets.property_types||[]" :key="f.value" :value="f.value">{{ f.value }} · {{ f.count }}</option></select></div>
          <div class="filter-block"><label>Recámaras mínimas</label><select v-model.number="search.min_bedrooms" @change="scheduleSearch(0)"><option :value="0">Cualquiera</option><option v-for="n in 6" :key="n" :value="n">{{ n }}+</option></select></div>
          <div class="filter-block"><label>Baños mínimos</label><select v-model.number="search.min_bathrooms" @change="scheduleSearch(0)"><option :value="0">Cualquiera</option><option v-for="n in [1,1.5,2,2.5,3,4]" :key="n" :value="n">{{ n }}+</option></select></div>
          <div class="filter-block"><label>Rating mínimo</label><select v-model.number="search.min_rating_x100" @change="scheduleSearch(0)"><option :value="0">Cualquiera</option><option :value="400">4.0+</option><option :value="425">4.25+</option><option :value="450">4.5+</option><option :value="475">4.75+</option></select></div>
          <div class="filter-block"><label>Temporada</label><select v-model="search.season" @change="scheduleSearch(0)"><option value="">Todas</option><option v-for="f in facets.seasons||[]" :key="f.value" :value="f.value">{{ f.value }} · {{ f.count }}</option></select></div>
          <div class="filter-block"><label>Modalidad</label><div class="segmented"><button :class="{'active':!search.booking_mode}" @click="setBooking('')">Todas</button><button :class="{'active':search.booking_mode==='fixed'}" @click="setBooking('fixed')">Fija</button><button :class="{'active':search.booking_mode==='floating'}" @click="setBooking('floating')">Flotante</button></div></div>
          <div class="filter-block price-filter"><label>Precio total</label><div class="range-fields"><div><small>Mín.</small><input inputmode="numeric" type="number" min="0" step="500" :value="minorToMajor(search.min_price_minor)" @change="setPrice('min',$event)"></div><span>—</span><div><small>Máx.</small><input inputmode="numeric" type="number" min="0" step="500" :value="minorToMajor(search.max_price_minor)" @change="setPrice('max',$event)"></div></div></div>
        </div>
        <div class="amenity-filter" v-if="amenityOptions.length"><label>Amenidades</label><div class="amenity-chips"><button v-for="a in amenityOptions" :key="a.name" class="filter-chip" :class="{'is-active':search.amenities.includes(a.name)}" @click="toggleAmenity(a.name)">{{ amenityLabel(a.name) }} <small>{{ a.count }}</small></button></div></div>
        <footer><span>{{ resultTotal }} estancias coinciden</span><div><button class="secondary" @click="clearAdvancedFilters">Restablecer</button><button class="primary" @click="applyFilters">Ver resultados</button></div></footer>
      </section>
    </Transition>

    <div v-if="error" class="error floating-error">{{ error }}</div>

    <section class="results-toolbar">
      <div><span class="eyebrow">Descubrimiento en tiempo real</span><h1>{{ resultsTitle }}</h1><p>{{ resultTotal }} estancia{{ resultTotal===1?'':'s' }} · {{ mapItems.length }} con ubicación en mapa</p></div>
      <div class="toolbar-actions">
        <label class="sort-control"><span>Ordenar</span><select v-model="search.sort" @change="loadInventory"><option value="relevance">Más relevantes</option><option value="price_asc">Precio menor</option><option value="price_desc">Precio mayor</option><option value="rating">Mejor rating</option><option value="soonest">Más próximas</option></select></label>
        <div class="view-toggle" role="group" aria-label="Vista"><button :class="{'active':view==='list'}" @click="setView('list')">Lista</button><button :class="{'active':view==='split'}" @click="setView('split')">Dividida</button><button :class="{'active':view==='map'}" @click="setView('map')">Mapa</button></div>
      </div>
    </section>

    <div v-if="activeFilterCount" class="active-filter-row">
      <button v-for="f in activeFilters" :key="f.key" class="active-filter" @click="removeActiveFilter(f)">{{ f.label }} <span>×</span></button>
    </div>

    <section class="explorer" :data-view="view">
      <div class="results-pane" v-show="view!=='map'">
        <div v-if="busy" class="skeleton-grid" aria-label="Cargando resultados"><div v-for="n in 6" :key="n" class="skeleton-card"><i></i><b></b><b></b><span></span></div></div>
        <TransitionGroup v-else name="results" tag="div" class="results-grid">
          <article class="property-card" v-for="p in inventory" :key="p.id" :data-id="p.id" :class="{'is-hovered':hoveredId===p.id,'is-saved':savedIds.includes(p.id)}" @mouseenter="hoverProperty(p)" @mouseleave="hoveredId=''">
            <div class="property-media" @click="focusOnMap(p)">
              <a :href="seoUrl(p)" class="media-link" :aria-label="'Ver '+p.resort"><img :src="p.image_url || fallback" :alt="p.unit_name" loading="lazy"></a>
              <div class="media-badges"><span v-if="p.rating" class="rating-badge">★ {{ p.rating.toFixed(1) }}</span><span v-if="p.booking_mode==='floating'" class="floating-badge">Flexible</span></div>
              <button class="save-button" :class="{'saved':savedIds.includes(p.id)}" @click.stop="toggleSave(p.id)" :aria-label="savedIds.includes(p.id)?'Quitar de guardados':'Guardar propiedad'">{{ savedIds.includes(p.id)?'♥':'♡' }}</button>
            </div>
            <div class="property-body">
              <div class="property-loc"><span>{{ [p.city,p.country].filter(Boolean).join(', ') || 'Destino vacacional' }}</span><button v-if="p.latitude!==null" @click="focusOnMap(p)">Ver mapa ↗</button></div>
              <a :href="seoUrl(p)" class="property-title">{{ p.resort }}</a>
              <div class="property-unit">{{ p.unit_name }}<template v-if="p.property_type"> · {{ p.property_type }}</template></div>
              <div class="fact-row"><span>{{ fmtDateShort(p.check_in) }} → {{ fmtDateShort(p.check_out) }}</span><span>Hasta {{ p.max_guests }}</span><span v-if="p.bedrooms">{{ p.bedrooms }} rec.</span><span v-if="p.bathrooms">{{ p.bathrooms }} baños</span></div>
              <div class="micro-chips" v-if="p.amenities?.length"><span v-for="a in p.amenities.slice(0,4)" :key="a">{{ amenityLabel(a) }}</span><span v-if="p.amenities.length>4">+{{ p.amenities.length-4 }}</span></div>
              <p class="description" v-if="p.short_description">{{ p.short_description }}</p>
              <div class="property-bottom"><div><small>Total estancia</small><strong>{{ money(p.price_minor,p.currency) }}</strong><span>Semana {{ p.week_number || '—' }}</span></div><div class="card-actions"><button class="compare-card-button" :class="{'is-active':compareIds.includes(p.id)}" @click="toggleCompare(p)" :aria-label="compareIds.includes(p.id)?'Quitar de comparación':'Comparar propiedad'">{{ compareIds.includes(p.id)?'✓ Compara':'⇄ Comparar' }}</button><button class="secondary compact" @click="openQuickView(p)">Vista rápida</button><button class="secondary compact" @click="add(p,false)">+ Carrito</button><button class="primary compact reserve-now" @click="reserveNow(p)"><span>Reservar</span><i aria-hidden="true">→</i></button></div></div>
            </div>
          </article>
        </TransitionGroup>
        <div v-if="!busy && inventory.length===0" class="empty-state"><div class="empty-orbit">⌕</div><h3>No encontramos una estancia exacta</h3><p>Prueba ampliar fechas, quitar un filtro o buscar otra ciudad. Tus filtros siguen guardados.</p><button class="secondary" @click="clearAdvancedFilters">Ampliar búsqueda</button></div>
        <button v-if="!busy && canLoadMore" class="load-more" @click="loadMore">Mostrar más estancias <span>↓</span></button>
      </div>

      <aside class="map-pane" v-show="view!=='list'" aria-label="Mapa de propiedades">
        <div ref="map" class="map-canvas"></div>
        <div class="map-top-controls">
          <button v-if="mapSearchPending" class="search-area-button" @click="searchMapArea"><span aria-hidden="true">⌕</span> Buscar en esta zona</button>
          <button class="map-reset" @click="fitMapToResults" title="Ver todos los resultados">◎</button>
        </div>
        <div v-if="!mapReady" class="map-loading"><i></i><span>Cargando mapa</span></div>
        <div class="map-legend"><span><i></i>{{ mapItems.length }} ubicaciones</span><small>Mueve el mapa para cambiar la zona</small></div>
      </aside>
    </section>
  </main>

  <Transition name="compare-rise">
    <section v-if="compareItems.length" class="compare-tray" aria-label="Comparador de propiedades">
      <div class="compare-mini-list"><div v-for="p in compareItems" :key="p.id" class="compare-mini"><img :src="p.image_url||fallback" :alt="p.unit_name"><span><strong>{{ p.resort }}</strong><small>{{ money(p.price_minor,p.currency) }} · {{ p.rating?('★ '+p.rating.toFixed(1)):'Sin rating' }}</small></span><button @click="toggleCompare(p)" :aria-label="'Quitar '+p.resort">×</button></div></div>
      <div class="compare-tray-actions"><span>{{ compareItems.length }}/4 seleccionadas</span><button class="secondary compact" @click="compareIds=[]">Limpiar</button><button class="primary compact" @click="compareOpen=true" :disabled="compareItems.length<2">Comparar ahora <span>→</span></button></div>
    </section>
  </Transition>

  <Transition name="shade"><div class="drawer-shade" v-if="compareOpen || quickView" @click="compareOpen=false;quickView=null"></div></Transition>
  <Transition name="modal-pop">
    <section v-if="compareOpen" class="decision-modal compare-modal" role="dialog" aria-modal="true" aria-label="Comparación de propiedades" @keydown.esc.window="compareOpen=false">
      <header class="decision-head"><div><span class="eyebrow">Decisión comprimida</span><h2>Compara sin abrir cuatro pestañas</h2></div><button class="close" @click="compareOpen=false">×</button></header>
      <div class="compare-grid">
        <article v-for="p in compareItems" :key="p.id" class="compare-column"><img :src="p.image_url||fallback" :alt="p.unit_name"><h3>{{ p.resort }}</h3><small>{{ p.unit_name }} · {{ [p.city,p.country].filter(Boolean).join(', ') }}</small><strong class="compare-price">{{ money(p.price_minor,p.currency) }}</strong><dl><div><dt>Rating</dt><dd>{{ p.rating?('★ '+p.rating.toFixed(1)):'—' }}</dd></div><div><dt>Huéspedes</dt><dd>{{ p.max_guests }}</dd></div><div><dt>Recámaras</dt><dd>{{ p.bedrooms||'—' }}</dd></div><div><dt>Baños</dt><dd>{{ p.bathrooms||'—' }}</dd></div><div><dt>Fechas</dt><dd>{{ fmtDateShort(p.check_in) }} → {{ fmtDateShort(p.check_out) }}</dd></div></dl><div class="micro-chips"><span v-for="a in (p.amenities||[]).slice(0,5)" :key="a">{{ amenityLabel(a) }}</span></div><button class="primary" @click="compareOpen=false;reserveNow(p)">Reservar esta <span>→</span></button></article>
      </div>
    </section>
  </Transition>

  <Transition name="modal-pop">
    <section v-if="quickView" class="decision-modal quick-modal" role="dialog" aria-modal="true" aria-label="Vista rápida de propiedad" @keydown.esc.window="quickView=null">
      <header class="decision-head"><div><span class="eyebrow">Vista rápida</span><h2>{{ quickView.resort }}</h2></div><button class="close" @click="quickView=null">×</button></header>
      <div class="quick-grid"><img class="quick-hero" :src="quickView.image_url||fallback" :alt="quickView.unit_name"><div class="quick-copy"><div class="property-loc"><span>{{ [quickView.city,quickView.country].filter(Boolean).join(', ') }}</span><span v-if="quickView.rating">★ {{ quickView.rating.toFixed(1) }}</span></div><h3>{{ quickView.unit_name }}<template v-if="quickView.property_type"> · {{ quickView.property_type }}</template></h3><p>{{ quickView.short_description || 'Estancia seleccionada de inventario FVS con disponibilidad en tiempo real.' }}</p><div class="quick-facts"><span>{{ fmtDate(quickView.check_in) }} → {{ fmtDate(quickView.check_out) }}</span><span>Hasta {{ quickView.max_guests||'—' }} huéspedes</span><span v-if="quickView.bedrooms">{{ quickView.bedrooms }} recámaras</span><span v-if="quickView.bathrooms">{{ quickView.bathrooms }} baños</span></div><div class="micro-chips"><span v-for="a in quickView.amenities||[]" :key="a">{{ amenityLabel(a) }}</span></div><div class="quick-price"><span><small>Total estancia</small><strong>{{ money(quickView.price_minor,quickView.currency) }}</strong></span><button class="primary" @click="quickView=null;reserveNow(quickView)">Reservar ahora <span>→</span></button></div></div></div>
      <section v-if="recommendations.length" class="recommend-strip"><header><strong>También encajan contigo</strong><small>Mismo destino/tipo, priorizadas por rating y precio</small></header><div><button v-for="r in recommendations" :key="r.id" class="recommend-card" @click="swapQuickView(r)"><img :src="r.image_url||fallback" :alt="r.unit_name"><span><strong>{{ r.resort }}</strong><small>{{ money(r.price_minor,r.currency) }} · {{ r.rating?('★ '+r.rating.toFixed(1)):r.property_type }}</small></span></button></div></section>
    </section>
  </Transition>

  <Transition name="shade"><div class="drawer-shade" v-if="drawer" @click="drawer=false"></div></Transition>
  <Transition name="drawer-slide">
    <aside id="reservation-drawer" class="drawer" v-if="drawer" role="dialog" aria-modal="true" aria-label="Carrito de reservaciones" @keydown.esc.window="drawer=false">
      <header class="drawer-head"><div><div class="eyebrow">Ruta rápida de reserva</div><strong>{{ cartCount }} propiedad{{ cartCount===1?'':'es' }}</strong><div class="pipeline"><span class="done">1 Selección</span><i></i><span :class="{'done':cartCount}">2 Revisión</span><i></i><span :class="{'done':cart?.status==='checkout'||cart?.status==='paid'}">3 Pago</span></div></div><button class="close" @click="drawer=false" aria-label="Cerrar carrito">×</button></header>
      <div class="drawer-body">
        <section class="cart-list">
          <div v-if="!cartCount" class="empty-state compact-empty"><div class="empty-orbit">＋</div><h3>Tu carrito está listo</h3><p>Agrega una estancia desde los resultados y continúa sin salir del flujo.</p></div>
          <TransitionGroup name="cart-pop" tag="div">
            <article class="cart-item" v-for="it in cart?.items || []" :key="it.slot_id">
              <img :src="it.image_url || fallback" :alt="it.unit_name"><div><div class="eyebrow">{{ [it.city,it.country].filter(Boolean).join(', ') }}</div><h3>{{ it.resort }} · {{ it.unit_name }}</h3><div class="meta">{{ fmtDate(it.check_in) }} — {{ fmtDate(it.check_out) }} · Semana {{ it.week_number || '—' }}</div><div class="guest-row"><label>Huéspedes</label><div class="mini-stepper"><button @click="setCartGuests(it,-1)" :disabled="cart?.status!=='open'">−</button><strong>{{ it.guests }}</strong><button @click="setCartGuests(it,1)" :disabled="cart?.status!=='open'">+</button></div></div><div class="cart-meta-row"><span class="hold">Hold {{ countdown(it.hold_expires_at) }}</span><span class="price">{{ money(it.price_minor,it.currency) }}</span></div></div><button class="remove" @click="remove(it.slot_id)" :disabled="cart?.status!=='open'" :aria-label="'Eliminar ' + it.unit_name">Eliminar</button>
            </article>
          </TransitionGroup>
        </section>
        <section class="summary">
          <div class="summary-sticky"><div class="eyebrow">Resumen inmediato</div><div class="line"><span>Estancias</span><strong>{{ cartCount }}</strong></div><div class="line"><span>Subtotal</span><span>{{ money(cart?.subtotal_minor ?? cart?.total_minor ?? 0,cart?.currency || 'MXN') }}</span></div><div v-if="cart?.addons_minor" class="line"><span>Experiencias y extras</span><span>+ {{ money(cart.addons_minor,cart.currency) }}</span></div><div v-if="cart?.discount_minor" class="line discount-line"><span>Ahorro <small v-if="cart?.promo_code">· {{ cart.promo_code }}</small></span><strong>− {{ money(cart.discount_minor,cart.currency) }}</strong></div><div class="line total"><span>Total</span><span>{{ money(cart?.total_minor || 0,cart?.currency || 'MXN') }}</span></div>
          <section v-if="commerce.addons.length && cartCount" class="addon-box"><header><div><strong>Mejora tu estancia</strong><small>Se agrega al mismo checkout y voucher</small></div><span>+</span></header><button v-for="a in commerce.addons" :key="a.code" class="addon-row" :class="{'is-active':cartAddonQty(a.code)>0}" @click="toggleAddon(a)"><span class="addon-icon">{{ a.icon || '✦' }}</span><span><strong>{{ a.name }}</strong><small>{{ a.description }}</small></span><span class="addon-price">{{ money(a.price_minor,a.currency) }}</span><span class="addon-toggle">{{ cartAddonQty(a.code)>0?'✓':'+' }}</span></button></section>
          <section v-if="cartCount" class="promo-box"><label>Promoción o referido</label><div class="promo-input"><input v-model.trim="promoInput" maxlength="64" placeholder="Código" @keydown.enter.prevent="applyCode"><button class="secondary compact" @click="applyCode" :disabled="promoBusy">{{ promoBusy?'Aplicando…':'Aplicar' }}</button></div><small v-if="cart?.promo_code || cart?.referral_code" class="promo-success">✓ {{ cart.promo_code || cart.referral_code }} aplicado al precio del core.</small><small v-else-if="promoMessage" :class="promoState==='error'?'promo-error':'promo-note'">{{ promoMessage }}</small></section>
          <section v-if="commerce.membership_plans.length && cartCount" class="membership-teaser"><span class="membership-orb">◇</span><div><strong>{{ commerce.membership_plans[0].name }}</strong><small>{{ commerce.membership_plans[0].description || 'Beneficios y recompensas para viajeros frecuentes.' }}</small></div><button class="text-button" @click="membershipOpen=!membershipOpen">{{ membershipOpen?'Ocultar':'Ver beneficios' }}</button><div v-if="membershipOpen" class="membership-details"><span v-for="b in commerce.membership_plans[0].benefits||[]" :key="String(b)">✓ {{ typeof b==='string'?b:(b.label||JSON.stringify(b)) }}</span><small>La membresía se muestra como propuesta comercial; no se añade automáticamente a esta reserva.</small></div></section>
          <div class="checkout-form" v-if="cartCount">
            <div class="field"><label>Email del huésped</label><input ref="checkoutEmail" type="email" v-model.trim="checkout.email" autocomplete="email" placeholder="huesped@correo.com"></div>
            <div class="field"><label>Pago</label><select v-model="checkout.provider" :disabled="cart?.status==='checkout' || stripeReady"><option value="stripe">Tarjeta · Stripe</option><option value="mercadopago">Mercado Pago</option></select></div>
            <label class="terms"><input type="checkbox" v-model="checkout.terms"><span>Acepto términos vigentes de renta, cancelación y uso.</span></label>
            <div v-if="checkoutMessage" :class="checkoutState==='confirmed'?'success':'notice'">{{ checkoutMessage }}</div><div v-if="checkoutError" class="error">{{ checkoutError }}</div><div id="stripe-element" v-show="stripeReady"></div>
            <button v-if="stripeReady" class="primary checkout-main" @click="confirmStripe" :disabled="paying">{{ paying?'Procesando…':'Confirmar pago' }}</button>
            <button v-else class="primary checkout-main" @click="beginCheckout" :disabled="paying || !checkout.terms || !checkout.email">{{ paying?'Preparando…':'Continuar al pago' }} <span>→</span></button>
            <small class="checkout-hint">Selección → revisión → pago, sin abandonar esta pantalla.</small>
          </div></div>
        </section>
      </div>
    </aside>
  </Transition>

  <button class="support-launcher" @click="toggleSupport" :aria-expanded="support.open?'true':'false'" aria-controls="support-panel"><span aria-hidden="true">✦</span><span>Ayuda</span><i v-if="support.threadId"></i></button>
  <Transition name="support-pop">
    <section id="support-panel" class="support-panel" v-if="support.open" role="dialog" aria-label="Soporte en línea" @keydown.esc.window="support.open=false">
      <header class="support-head"><div><strong>Soporte FVS</strong><small>{{ supportStatusLabel }}</small></div><button class="close" @click="support.open=false" aria-label="Cerrar soporte">×</button></header>
      <div v-if="!support.threadId" class="support-start"><p>La IA resuelve dudas comunes y transfiere a un agente humano cuando necesitas una acción sobre tu reserva.</p><div class="field"><label>Email (opcional)</label><input type="email" v-model.trim="support.email" autocomplete="email"></div><div class="field"><label>¿En qué podemos ayudarte?</label><textarea v-model.trim="support.input" rows="4" maxlength="4000" placeholder="Describe tu duda"></textarea></div><button class="primary" @click="startSupport" :disabled="support.sending || !support.input">{{ support.sending?'Abriendo…':'Iniciar conversación' }}</button></div>
      <template v-else><div class="support-messages" ref="supportMessages"><article v-for="m in support.thread?.messages || []" :key="m.id" class="support-message" :class="'from-'+m.sender_type"><div class="support-author">{{ m.sender_type==='customer'?'Tú':m.sender_type==='ai'?'Asistente IA':m.sender_type==='agent'?'Agente FVS':'FVS' }}</div><div>{{ m.body }}</div></article><div v-if="support.waiting" class="support-typing"><i></i><i></i><i></i></div></div><div v-if="support.thread?.handoff_reason" class="notice support-handoff">Tu caso fue escalado a un agente humano. Conservamos todo el contexto.</div><form class="support-compose" @submit.prevent="sendSupport"><textarea v-model.trim="support.input" rows="2" maxlength="4000" placeholder="Escribe tu mensaje"></textarea><button class="primary" :disabled="support.sending || !support.input">Enviar</button></form></template><div v-if="support.error" class="error support-error">{{ support.error }}</div>
    </section>
  </Transition>
</template>
<script>
const {api,pad,debounce,localJson,savedView}=window.FVS_UI;
export default {
  data(){return {
    search:{q:'',check_in:'',check_out:'',guests:2,country:'',city:'',resort:'',property_type:'',season:'',booking_mode:'',amenities:[],min_bedrooms:0,min_bathrooms:0,min_rating_x100:0,min_price_minor:0,max_price_minor:0,sort:'relevance'},
    inventory:[],facets:{countries:[],cities:[],property_types:[],seasons:[],booking_modes:[]},resultTotal:0,offset:0,pageSize:60,busy:false,error:'',showFilters:false,
    suggestions:[],suggestionsOpen:false,suggesting:false,suggestionIndex:-1,view:savedView(),hoveredId:'',savedIds:(()=>{const v=localJson('fvs_saved',[]);return Array.isArray(v)?v.filter(x=>typeof x==='string').slice(0,500):[]})(),
    map:null,mapReady:false,mapMarkers:[],mapBounds:null,mapBoundsPending:null,mapSearchPending:false,mapUserMoved:false,
    cart:null,cartId:localStorage.getItem('fvs_cart_id')||'',drawer:false,paying:false,now:Date.now(),timer:null,checkout:{email:'',provider:'stripe',terms:false},checkoutMessage:'',checkoutError:'',checkoutState:'',stripe:null,elements:null,paymentElement:null,stripeReady:false,
    support:{open:false,threadId:localStorage.getItem('fvs_support_thread_id')||'',thread:null,email:'',input:'',sending:false,waiting:false,error:'',poll:null},
    commerce:{collections:[],addons:[],membership_plans:[],promotions:[]},activeCollection:null,compareIds:(()=>{const v=localJson('fvs_compare',[]);return Array.isArray(v)?v.filter(x=>typeof x==='string').slice(0,4):[]})(),compareOpen:false,quickView:null,recommendations:[],promoInput:'',promoBusy:false,promoMessage:'',promoState:'',membershipOpen:false,
    socialOrigin:{channel:'',token:'',slot:'',collection:'',campaign_id:'',creative_id:'',promo:''},originBound:false,
    fallback:window.FVS_UI.fallback
  }},
  computed:{
    cartCount(){return this.cart?.items?.length||0},
    compareItems(){return this.compareIds.map(id=>this.inventory.find(p=>p.id===id)).filter(Boolean)},
    mapItems(){return this.inventory.filter(p=>Number.isFinite(Number(p.latitude))&&Number.isFinite(Number(p.longitude)))},
    canLoadMore(){return this.inventory.length<this.resultTotal},
    topCountries(){return (this.facets.countries||[]).slice(0,4)},topCities(){return (this.facets.cities||[]).slice(0,5)},topTypes(){return (this.facets.property_types||[]).slice(0,4)},
    amenityOptions(){const m=new Map;for(const p of this.inventory)for(const a of p.amenities||[])m.set(a,(m.get(a)||0)+1);return [...m].sort((a,b)=>b[1]-a[1]).slice(0,14).map(([name,count])=>({name,count}))},
    activeFilters(){const a=[];const push=(key,label,v)=>{if(v)a.push({key,label:`${label}: ${v}`})};push('country','País',this.search.country);push('city','Ciudad',this.search.city);push('resort','Resort',this.search.resort);push('property_type','Tipo',this.search.property_type);push('season','Temporada',this.search.season);push('booking_mode','Modalidad',this.search.booking_mode==='fixed'?'Fija':this.search.booking_mode==='floating'?'Flotante':'');if(this.search.min_bedrooms)a.push({key:'min_bedrooms',label:`${this.search.min_bedrooms}+ rec.`});if(this.search.min_bathrooms)a.push({key:'min_bathrooms',label:`${this.search.min_bathrooms}+ baños`});if(this.search.min_rating_x100)a.push({key:'min_rating_x100',label:`★ ${(this.search.min_rating_x100/100).toFixed(2)}+`});if(this.search.min_price_minor)a.push({key:'min_price_minor',label:`Desde ${this.money(this.search.min_price_minor,'MXN')}`});if(this.search.max_price_minor)a.push({key:'max_price_minor',label:`Hasta ${this.money(this.search.max_price_minor,'MXN')}`});for(const x of this.search.amenities)a.push({key:'amenity:'+x,label:this.amenityLabel(x)});if(this.mapBounds)a.push({key:'bbox',label:'Área del mapa'});return a},
    activeFilterCount(){return this.activeFilters.length+(this.search.q?1:0)},advancedFilterCount(){return this.activeFilters.length},
    resultsTitle(){if(this.search.resort)return this.search.resort;if(this.search.city)return `Estancias en ${this.search.city}`;if(this.search.country)return `Estancias en ${this.search.country}`;if(this.search.q)return `Resultados para “${this.search.q}”`;return 'Explora estancias disponibles'},
    supportStatusLabel(){const s=this.support.thread?.status;if(!s)return 'IA + agentes humanos';return s==='waiting_human'?'En cola para agente':s==='in_progress'?'Agente conectado':s==='waiting_customer'?'Esperando tu respuesta':s==='resolved'?'Caso resuelto':'Asistencia activa'}
  },
  async mounted(){this.hydrateSearch();this.hydrateCommerceIntent();const d=new Date(),ci=new Date(d.getTime()+30*86400000),co=new Date(d.getTime()+37*86400000);if(!this.search.check_in)this.search.check_in=`${ci.getFullYear()}-${pad(ci.getMonth()+1)}-${pad(ci.getDate())}`;if(!this.search.check_out)this.search.check_out=`${co.getFullYear()}-${pad(co.getMonth()+1)}-${pad(co.getDate())}`;this.timer=setInterval(()=>this.now=Date.now(),1000);this._suggestDebounced=debounce(()=>this.fetchSuggestions(),180);this._searchDebounced=debounce(()=>this.loadInventory(),320);window.addEventListener('keydown',this.onGlobalKey);await Promise.all([this.loadCommerce(),this.restoreCart()]);if(this.cartId)await this.bindOrigin();const returned=new URLSearchParams(location.search).has('checkout')||new URLSearchParams(location.search).has('payment_intent');if(returned||['checkout','paid','manual_review'].includes(this.cart?.status)){this.drawer=true;await this.resumeCheckout();}if(this.socialOrigin.collection)await this.openCollectionBySlug(this.socialOrigin.collection,false);else await this.loadInventory();if(this.view!=='list')await this.initMap();if(this.support.threadId)this.refreshSupport();},
  beforeUnmount(){clearInterval(this.timer);if(this.support.poll)clearInterval(this.support.poll);window.removeEventListener('keydown',this.onGlobalKey);this.destroyMap()},
  methods:{
    ...window.FVS_UI.methods,
    track(type,extra={}){try{window.FVS_ATTR?.track(type,extra)}catch{}},
    hydrateCommerceIntent(){const q=new URLSearchParams(location.search);this.socialOrigin={channel:q.get('utm_source')||'',token:q.get('fvs_social')||'',slot:q.get('slot')||'',collection:q.get('collection')||'',campaign_id:q.get('campaign_id')||'',creative_id:q.get('creative_id')||'',promo:q.get('promo')||''};if(this.socialOrigin.token&&!this.socialOrigin.channel)this.socialOrigin.channel='social';},
    async loadCommerce(){try{const c=await api('/api/v1/commerce/home');this.commerce={collections:c.collections||[],addons:c.addons||[],membership_plans:c.membership_plans||[],promotions:c.promotions||[]};if(!this.promoInput&&this.socialOrigin.promo)this.promoInput=this.socialOrigin.promo}catch(e){console.warn('commerce unavailable',e)}},
    async openCollection(c){return this.openCollectionBySlug(c.slug,true)},
    async openCollectionBySlug(slug,scroll=true){try{const c=await api('/api/v1/commerce/collections/'+encodeURIComponent(slug));this.activeCollection=c;this.applyCollectionFilter(c.filter||{});if((c.items||[]).length){const ids=new Set((c.items||[]).map(x=>x.id));await this.loadInventory();this.inventory.sort((a,b)=>(ids.has(b.id)?1:0)-(ids.has(a.id)?1:0));}else await this.loadInventory();this.track('collection_view',{collection:slug});if(scroll)this.$nextTick(()=>document.querySelector('.results-toolbar')?.scrollIntoView({behavior:'smooth',block:'start'}))}catch(e){this.error='No se pudo abrir la colección.'}},
    applyCollectionFilter(f){const keys=['country','city','resort','property_type','season','booking_mode','sort'];for(const k of keys)if(typeof f[k]==='string')this.search[k]=f[k];if(Array.isArray(f.amenities))this.search.amenities=f.amenities.slice(0,20);for(const k of ['guests','min_bedrooms','min_bathrooms','min_rating_x100','min_price_minor','max_price_minor'])if(Number.isFinite(Number(f[k])))this.search[k]=Number(f[k]);},
    toggleCompare(p){const id=p.id;if(this.compareIds.includes(id))this.compareIds=this.compareIds.filter(x=>x!==id);else if(this.compareIds.length<4)this.compareIds=[...this.compareIds,id];else{this.compareIds=[...this.compareIds.slice(1),id];this.error='El comparador admite 4 propiedades; reemplazamos la más antigua.'}localStorage.setItem('fvs_compare',JSON.stringify(this.compareIds));},
    async openQuickView(p){this.quickView=p;this.recommendations=[];try{const r=await api(`/api/v1/commerce/recommendations?slot_id=${encodeURIComponent(p.id)}&limit=6`);this.recommendations=r.items||[]}catch{}this.track('quick_view',{slot_id:p.id})},
    async swapQuickView(p){this.quickView=p;await this.openQuickView(p)},
    cartAddonQty(code){return Number((this.cart?.addons||[]).find(a=>a.code===code)?.quantity||0)},
    async toggleAddon(a){if(!this.cartId||this.cart?.status!=='open')return;const q=this.cartAddonQty(a.code)>0?0:1;try{this.cart=await api(`/api/v1/cart/${this.cartId}/addons/${encodeURIComponent(a.code)}`,{method:'PUT',body:JSON.stringify({quantity:q})});this.resetPaymentElement();this.track('addon',{code:a.code,quantity:q})}catch{this.checkoutError='No se pudo actualizar el extra.'}},
    async applyCode(){if(!this.cartId)return;this.promoBusy=true;this.promoMessage='';this.promoState='';try{this.cart=await api(`/api/v1/cart/${this.cartId}/code`,{method:'POST',body:JSON.stringify({code:this.promoInput})});this.promoState='ok';this.promoMessage='Código validado en el precio del servidor.';this.resetPaymentElement();this.track('promotion',{code:this.promoInput||null,discount_minor:Number(this.cart?.discount_minor||0)})}catch(e){this.promoState='error';this.promoMessage=e.status===409?'El código no aplica a este carrito o ya venció.':'No se pudo validar el código.'}finally{this.promoBusy=false}},
    resetPaymentElement(){this.stripeReady=false;if(this.paymentElement){this.paymentElement.destroy();this.paymentElement=null}this.elements=null;this.checkout.terms=false},
    async bindOrigin(){if(!this.cartId||this.originBound||!(this.socialOrigin.channel||this.socialOrigin.token||this.socialOrigin.campaign_id||this.socialOrigin.creative_id))return;try{this.cart=await api(`/api/v1/cart/${this.cartId}/origin`,{method:'POST',body:JSON.stringify({channel:this.socialOrigin.channel||'other',campaign_id:this.socialOrigin.campaign_id,creative_id:this.socialOrigin.creative_id,social_token:this.socialOrigin.token})});this.originBound=true;if(this.socialOrigin.promo&&!this.cart?.promo_code&&!this.cart?.referral_code){this.promoInput=this.socialOrigin.promo;await this.applyCode()}}catch(e){console.warn('origin bind failed',e)}},
    async reserveSocialSlot(){if(!this.socialOrigin.slot)return;await this.add({id:this.socialOrigin.slot},true)},
    hydrateSearch(){const q=new URLSearchParams(location.search);for(const k of ['q','check_in','check_out','country','city','resort','property_type','season','booking_mode','sort'])if(q.has(k))this.search[k]=q.get(k)||'';if(q.has('guests'))this.search.guests=Math.max(1,Math.min(64,Number(q.get('guests'))||2));if(q.has('amenities'))this.search.amenities=(q.get('amenities')||'').split(',').filter(Boolean);if(q.has('min_bedrooms'))this.search.min_bedrooms=Math.max(0,Number(q.get('min_bedrooms'))||0);if(q.has('min_bathrooms'))this.search.min_bathrooms=Math.max(0,Number(q.get('min_bathrooms'))||0);if(q.has('min_rating_x100'))this.search.min_rating_x100=Math.max(0,Number(q.get('min_rating_x100'))||0);if(q.has('min_price_minor'))this.search.min_price_minor=Number(q.get('min_price_minor'))||0;if(q.has('max_price_minor'))this.search.max_price_minor=Number(q.get('max_price_minor'))||0;if(q.has('bbox')){const v=(q.get('bbox')||'').split(',').map(Number);if(v.length===4&&v.every(Number.isFinite))this.mapBounds=v}},
    syncUrl(){const p=this.buildParams(false);for(const [k,v] of [...p])if(!v)p.delete(k);const qs=p.toString();history.replaceState(null,'',location.pathname+(qs?'?'+qs:''))},
    buildParams(includePaging=true){const p=new URLSearchParams;for(const k of ['q','check_in','check_out','country','city','resort','property_type','season','booking_mode','sort'])if(this.search[k])p.set(k,this.search[k]);p.set('guests',String(this.search.guests));if(this.search.amenities.length)p.set('amenities',this.search.amenities.join(','));if(this.search.min_bedrooms)p.set('min_bedrooms',String(this.search.min_bedrooms));if(this.search.min_bathrooms)p.set('min_bathrooms',String(this.search.min_bathrooms));if(this.search.min_rating_x100)p.set('min_rating_x100',String(this.search.min_rating_x100));if(this.search.min_price_minor)p.set('min_price_minor',String(this.search.min_price_minor));if(this.search.max_price_minor)p.set('max_price_minor',String(this.search.max_price_minor));if(this.mapBounds)p.set('bbox',this.mapBounds.join(','));if(includePaging){p.set('limit',String(this.pageSize));p.set('offset',String(this.offset))}return p},
    async loadInventory(opts={}){if(opts.reset!==false)this.offset=0;this.busy=true;this.error='';try{const r=await api('/api/v1/inventory?'+this.buildParams(true));this.inventory=r.items||[];this.resultTotal=Number(r.total||this.inventory.length);this.facets=r.facets||this.facets;this.syncUrl();this.$nextTick(()=>{this.updateMapMarkers();if(opts.fit!==false&&!this.mapBounds)this.fitMapToResults()});this.track('search',{results:this.resultTotal,query:this.search.q||null,country:this.search.country||null,city:this.search.city||null})}catch(e){console.error(e);this.error='No se pudo consultar el inventario.'}finally{this.busy=false}},
    async loadMore(){this.offset=this.inventory.length;this.busy=true;try{const r=await api('/api/v1/inventory?'+this.buildParams(true));const seen=new Set(this.inventory.map(x=>x.id));this.inventory.push(...(r.items||[]).filter(x=>!seen.has(x.id)));this.resultTotal=Number(r.total||this.resultTotal);this.facets=r.facets||this.facets;this.$nextTick(()=>this.updateMapMarkers())}catch{this.error='No se pudieron cargar más resultados.'}finally{this.busy=false}},
    scheduleSearch(ms=320){if(ms===0)this.loadInventory();else this._searchDebounced?.()},
    onQueryInput(){this.suggestionIndex=-1;this.openSuggestions();this._suggestDebounced?.()},openSuggestions(){this.suggestionsOpen=true;if(this.search.q&&this.suggestions.length===0)this._suggestDebounced?.()},clearQuery(){this.search.q='';this.suggestions=[];this.suggestionsOpen=false;this.loadInventory()},
    async fetchSuggestions(){const q=this.search.q.trim();if(!q){this.suggestions=[];return}this.suggesting=true;try{const r=await api('/api/v1/search/suggest?q='+encodeURIComponent(q)+'&limit=12');this.suggestions=r.suggestions||[];this.suggestionsOpen=true}catch{this.suggestions=[]}finally{this.suggesting=false}},
    moveSuggestion(d){if(!this.suggestions.length)return;this.suggestionIndex=(this.suggestionIndex+d+this.suggestions.length)%this.suggestions.length},acceptHighlighted(){if(this.suggestionIndex>=0&&this.suggestions[this.suggestionIndex])this.selectSuggestion(this.suggestions[this.suggestionIndex]);else this.searchFreeText()},
    selectSuggestion(s){if(s.type==='country'){this.search.country=s.label;this.search.city='';this.search.resort='';this.search.q=''}else if(s.type==='city'){this.search.city=s.label;this.search.country=s.subtitle||this.search.country;this.search.resort='';this.search.q=''}else if(s.type==='resort'){this.search.resort=s.label;this.search.q=''}else this.search.q=s.label;this.suggestionsOpen=false;this.mapBounds=null;this.loadInventory()},searchFreeText(){this.suggestionsOpen=false;this.mapBounds=null;this.loadInventory()},
    stepGuests(d){this.search.guests=Math.max(1,Math.min(64,this.search.guests+d));this.scheduleSearch()},toggleExact(k,v){this.search[k]=this.search[k]===v?'':v;if(k==='country'&&!this.search[k])this.search.city='';this.mapBounds=null;this.loadInventory()},onHierarchyChange(k){if(k==='country')this.search.city='';this.mapBounds=null;this.loadInventory()},
    setBooking(v){this.search.booking_mode=v;this.scheduleSearch(0)},setPrice(which,e){const val=Math.max(0,Number(e.target.value)||0)*100;this.search[which==='min'?'min_price_minor':'max_price_minor']=Math.round(val);this.scheduleSearch()},toggleAmenity(a){const i=this.search.amenities.indexOf(a);if(i>=0)this.search.amenities.splice(i,1);else this.search.amenities.push(a);this.scheduleSearch()},
    clearAdvancedFilters(){Object.assign(this.search,{country:'',city:'',resort:'',property_type:'',season:'',booking_mode:'',amenities:[],min_bedrooms:0,min_bathrooms:0,min_rating_x100:0,min_price_minor:0,max_price_minor:0});this.mapBounds=null;this.mapBoundsPending=null;this.mapSearchPending=false;this.loadInventory()},applyFilters(){this.showFilters=false;this.loadInventory()},
    removeActiveFilter(f){if(f.key.startsWith('amenity:')){const a=f.key.slice(8);this.search.amenities=this.search.amenities.filter(x=>x!==a)}else if(f.key==='bbox'){this.mapBounds=null;this.mapBoundsPending=null;this.mapSearchPending=false}else this.search[f.key]=typeof this.search[f.key]==='number'?0:'';this.loadInventory()},
    focusSearch(){this.$refs.omni?.focus()},onGlobalKey(e){if(['INPUT','TEXTAREA','SELECT'].includes(document.activeElement?.tagName))return;if(e.key==='/'){e.preventDefault();this.focusSearch()}else if(e.key.toLowerCase()==='m'){e.preventDefault();this.toggleView()}else if(e.key.toLowerCase()==='c'){e.preventDefault();this.drawer=true}},toggleView(){this.setView(this.view==='map'?'list':'map')},async setView(v){this.view=v;localStorage.setItem('fvs_view',v);if(v!=='list'){await this.initMap();this.$nextTick(()=>{this.map?.resize();this.updateMapMarkers();this.fitMapToResults()})}},
    async initMap(){if(this.map)return;try{const ml=await (window.FVS_LOAD_MAPLIBRE?window.FVS_LOAD_MAPLIBRE():Promise.resolve(window.maplibregl));if(!ml||!this.$refs.map)return;const style=window.FVS_MAP_STYLE_URL||{version:8,sources:{osm:{type:'raster',tiles:['https://tile.openstreetmap.org/{z}/{x}/{y}.png'],tileSize:256,attribution:'© OpenStreetMap contributors'}},layers:[{id:'osm',type:'raster',source:'osm'}]};this.map=new ml.Map({container:this.$refs.map,style,center:[-102,23.6],zoom:4,minZoom:2,maxZoom:18,attributionControl:true});this.map.addControl(new ml.NavigationControl({showCompass:false}),'bottom-right');this.map.on('load',()=>{this.mapReady=true;this.updateMapMarkers();this.fitMapToResults()});this.map.on('movestart',e=>{if(e.originalEvent)this.mapUserMoved=true});this.map.on('moveend',()=>{if(!this.mapUserMoved)return;this.mapUserMoved=false;const b=this.map.getBounds();this.mapBoundsPending=[b.getSouth(),b.getWest(),b.getNorth(),b.getEast()].map(v=>Number(v.toFixed(6)));this.mapSearchPending=true})}catch(e){console.warn('map unavailable',e)}},
    destroyMap(){for(const m of this.mapMarkers)m.remove();this.mapMarkers=[];if(this.map){this.map.remove();this.map=null}},updateMapMarkers(){if(!this.map||!window.maplibregl)return;for(const m of this.mapMarkers)m.remove();this.mapMarkers=[];for(const p of this.mapItems){const el=document.createElement('button');el.className='price-marker'+(this.hoveredId===p.id?' is-active':'');el.type='button';el.textContent=this.shortMoney(p.price_minor,p.currency);el.setAttribute('aria-label',`${p.resort}, ${this.money(p.price_minor,p.currency)}`);el.addEventListener('click',()=>{this.hoveredId=p.id;this.scrollToCard(p.id)});const marker=new window.maplibregl.Marker({element:el,anchor:'bottom'}).setLngLat([Number(p.longitude),Number(p.latitude)]).addTo(this.map);marker._fvsId=p.id;this.mapMarkers.push(marker)}},
    fitMapToResults(){if(!this.map||!this.mapItems.length)return;const ml=window.maplibregl;const b=new ml.LngLatBounds;for(const p of this.mapItems)b.extend([Number(p.longitude),Number(p.latitude)]);if(!b.isEmpty()){this.mapUserMoved=false;this.map.fitBounds(b,{padding:70,maxZoom:12,duration:650})}},searchMapArea(){if(!this.mapBoundsPending)return;this.mapBounds=this.mapBoundsPending;this.mapSearchPending=false;this.loadInventory({fit:false})},focusOnMap(p){if(p.latitude===null||p.longitude===null)return;if(this.view==='list')this.setView('split');this.$nextTick(()=>{this.map?.flyTo({center:[Number(p.longitude),Number(p.latitude)],zoom:12,duration:700});this.hoveredId=p.id;this.refreshMarkerStates()})},hoverProperty(p){this.hoveredId=p.id;this.refreshMarkerStates()},refreshMarkerStates(){for(const m of this.mapMarkers)m.getElement().classList.toggle('is-active',m._fvsId===this.hoveredId)},scrollToCard(id){if(this.view==='map')this.setView('split');this.$nextTick(()=>document.querySelector(`.property-card[data-id="${id}"]`)?.scrollIntoView({behavior:'smooth',block:'center'}))},
    toggleSave(id){this.savedIds=this.savedIds.includes(id)?this.savedIds.filter(x=>x!==id):[...this.savedIds,id];localStorage.setItem('fvs_saved',JSON.stringify(this.savedIds));},
    async ensureCart(){if(this.cartId)return;const c=await api('/api/v1/cart',{method:'POST',body:'{}'});this.cartId=c.cart_id;localStorage.setItem('fvs_cart_id',this.cartId);await this.bindOrigin()},async restoreCart(){if(!this.cartId)return;try{this.cart=await api(`/api/v1/cart/${this.cartId}`)}catch(e){if(e.status===401||e.status===404){localStorage.removeItem('fvs_cart_id');this.cartId='';this.cart=null;}}},
    async add(p,open=true){try{await this.ensureCart();this.cart=await api(`/api/v1/cart/${this.cartId}/items`,{method:'POST',body:JSON.stringify({slot_id:p.id,guests:this.search.guests})});if(open)this.drawer=true;this.track('cart',{slot_id:p.id});if(open)this.$nextTick(()=>this.$refs.checkoutEmail?.focus())}catch(e){this.error=e.status===409?'La estancia acaba de ser tomada o ya no está disponible.':'No se pudo agregar la estancia.';await this.loadInventory()}},async reserveNow(p){await this.add(p,true)},
    async setCartGuests(it,d){const guests=Math.max(1,Math.min(Number(it.max_guests)||1,Number(it.guests||1)+d));try{this.cart=await api(`/api/v1/cart/${this.cartId}/items`,{method:'POST',body:JSON.stringify({slot_id:it.slot_id,guests})})}catch(e){this.checkoutError=e.status===409?'La estancia cambió de disponibilidad.':'No se pudo actualizar huéspedes.';await this.restoreCart()}},
    async remove(id){try{this.cart=await api(`/api/v1/cart/${this.cartId}/items/${id}`,{method:'DELETE'});this.resetPaymentElement()}catch{this.checkoutError='No se pudo eliminar la estancia.'}},
    async beginCheckout(){this.paying=true;this.checkoutError='';this.checkoutMessage='';try{this.track('checkout',{value_minor:Number(this.cart?.total_minor||0),currency:this.cart?.currency||'MXN'});const a=await api(`/api/v1/cart/${this.cartId}/checkout`,{method:'POST',body:JSON.stringify({provider:this.checkout.provider,email:this.checkout.email,terms_accepted:this.checkout.terms})});await this.handleAttempt(a)}catch(e){this.checkoutError=e.status===409?'El carrito cambió o alguna estancia dejó de estar disponible.':'No fue posible preparar el pago.'}finally{this.paying=false}},
    async resumeCheckout(){if(!this.cartId)return;try{const a=await api(`/api/v1/cart/${this.cartId}/checkout/status`);await this.handleAttempt(a)}catch(e){if(e.status!==404)this.checkoutError='No se pudo recuperar el estado del pago.'}},
    async handleAttempt(a){if(a.status==='quote_refreshed'){await this.restoreCart();this.checkout.terms=false;this.checkoutState='quote_refreshed';this.checkoutMessage='La retención anterior venció. Actualizamos disponibilidad y cotización; revisa el total antes de pagar.';this.stripeReady=false;if(this.paymentElement){this.paymentElement.destroy();this.paymentElement=null}return}if(a.status==='paid'||a.status==='confirmed'){this.checkoutState='confirmed';this.checkoutMessage=`Reserva confirmada${a.order_id?' · Orden '+a.order_id:''}. Tu voucher se procesará automáticamente.`;this.track('booking',{order_id:a.order_id||null,value_minor:Number(a.amount_minor||this.cart?.total_minor||0),currency:a.currency||this.cart?.currency||'MXN'});this.stripeReady=false;if(this.paymentElement){this.paymentElement.destroy();this.paymentElement=null}if(this.cart)this.cart.status='paid';localStorage.removeItem('fvs_cart_id');this.cartId='';return}if(a.status==='manual_review'){this.checkoutState='manual_review';if(this.cart)this.cart.status='manual_review';this.checkoutMessage='El pago requiere conciliación manual; no se creó una reserva parcial.';return}if(a.status==='provider_reconciliation_pending'){this.checkoutMessage='Estamos conciliando el intento de pago. Puedes recargar sin crear otro cobro.';return}if(['creating','pending'].includes(a.status)&&this.cart)this.cart.status='checkout';if(a.provider==='mercadopago'&&a.redirect_url){location.assign(a.redirect_url);return}if(a.provider==='stripe'&&a.client_secret){if(!window.Stripe){this.checkoutError='Stripe.js no está disponible.';return}const key=document.querySelector('meta[name="stripe-key"]')?.content||window.FVS_STRIPE_PK||'';if(!key){this.checkoutError='Falta configurar la clave pública de Stripe.';return}this.stripe=window.Stripe(key);if(this.paymentElement){this.paymentElement.destroy();this.paymentElement=null}this.elements=this.stripe.elements({clientSecret:a.client_secret});this.paymentElement=this.elements.create('payment');this.paymentElement.mount('#stripe-element');this.stripeReady=true;this.checkoutMessage='El intento de pago quedó fijado a este carrito y total.';}},
    async confirmStripe(){if(!this.stripe||!this.elements)return;this.paying=true;this.checkoutError='';const r=await this.stripe.confirmPayment({elements:this.elements,confirmParams:{return_url:location.origin+'/?checkout=return'}});if(r.error)this.checkoutError=r.error.message||'No se pudo confirmar el pago.';this.paying=false},
    async toggleSupport(){this.support.open=!this.support.open;if(this.support.open&&this.support.threadId){await this.refreshSupport();this.beginSupportPolling()}},beginSupportPolling(){if(this.support.poll)clearInterval(this.support.poll);this.support.poll=setInterval(()=>{if(this.support.open&&this.support.threadId)this.refreshSupport(true)},4000)},
    async startSupport(){this.support.sending=true;this.support.error='';const first=this.support.input;try{const r=await api('/api/v1/support',{method:'POST',body:JSON.stringify({email:this.support.email,subject:'Soporte en línea FVS',cart_id:this.cartId||null})});this.support.threadId=r.thread_id;localStorage.setItem('fvs_support_thread_id',r.thread_id);this.support.input='';await api(`/api/v1/support/${r.thread_id}/messages`,{method:'POST',body:JSON.stringify({body:first})});this.support.waiting=true;await this.refreshSupport();this.beginSupportPolling()}catch{this.support.error='No pudimos abrir el chat. Intenta nuevamente.'}finally{this.support.sending=false}},
    async sendSupport(){if(!this.support.threadId||!this.support.input)return;const body=this.support.input;this.support.input='';this.support.sending=true;this.support.error='';try{this.support.thread=await api(`/api/v1/support/${this.support.threadId}/messages`,{method:'POST',body:JSON.stringify({body})});this.support.waiting=!!this.support.thread?.ai_enabled;this.scrollSupport()}catch{this.support.input=body;this.support.error='No se pudo enviar el mensaje.'}finally{this.support.sending=false}},
    async refreshSupport(silent=false){if(!this.support.threadId)return;try{const before=this.support.thread?.messages?.length||0;this.support.thread=await api(`/api/v1/support/${this.support.threadId}`);this.support.waiting=['open','in_progress'].includes(this.support.thread?.status)&&this.support.thread?.ai_enabled;const after=this.support.thread?.messages?.length||0;if(after!==before)this.scrollSupport()}catch(e){if(e.status===401||e.status===404){localStorage.removeItem('fvs_support_thread_id');this.support.threadId='';this.support.thread=null}else if(!silent)this.support.error='No se pudo recuperar la conversación.'}},scrollSupport(){this.$nextTick(()=>{const el=this.$refs.supportMessages;if(el)el.scrollTop=el.scrollHeight})}
  }
}
</script>
