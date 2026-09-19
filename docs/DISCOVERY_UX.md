# Discovery y arquitectura UX

## Objetivo

Convertir el catálogo en una superficie de descubrimiento de alta densidad sin introducir Node ni un cluster de búsqueda adicional. El mismo contrato alimenta omnibox, facetas, lista, mapa y URL state para evitar divergencias entre interfaces.

## Pipeline comprimido

### 1. Descubrimiento libre
`omnibox → autosuggest → resultado/facetas → reservar`

Un huésped puede escribir país, ciudad, resort o propiedad. Una sugerencia aplica el filtro correcto y dispara la consulta sin obligarlo a navegar una taxonomía previa.

### 2. Mapa
`resultado → mapa → mover viewport → Buscar en esta zona → reservar`

El mapa no mantiene un inventario paralelo. Convierte su viewport en `bbox=min_lat,min_lng,max_lat,max_lng`, vuelve a consultar el core y mantiene ranking/filtros/availability consistentes.

### 3. Reserva rápida
`card → Reservar → cart hold → review/terms → checkout`

El botón Reservar crea/reutiliza carrito, toma el hold y abre el drawer en el contexto de checkout. El botón + Carrito conserva el modo exploración para compras multipropiedad.

## Contrato search

`GET /api/v1/inventory` soporta:

- `q`
- `check_in`, `check_out`, `guests`
- `country`, `city`, `resort`, `property_type`
- `season`, `booking_mode`
- `amenities=a,b,c`
- `min_bedrooms`, `min_bathrooms`, `min_rating_x100`
- `min_price_minor`, `max_price_minor`
- `bbox=min_lat,min_lng,max_lat,max_lng`
- `sort=relevance|price_asc|price_desc|rating|soonest`
- `limit`, `offset`

La respuesta contiene `items`, `total`, `facets`, `limit` y `offset`.

`GET /api/v1/search/suggest?q=...` devuelve sugerencias tipadas `country|city|resort|property`.

## Ranking

Cuando existe `q`, FVS combina:

1. score `MATCH(search_text) AGAINST(...)`;
2. boost por resort exacto;
3. boost por ciudad exacta;
4. boost por prefijo de resort/ciudad;
5. precio como desempate estable.

Existe fallback `LIKE` para términos que MySQL full-text no rankea bien. Los filtros de disponibilidad, ocupación y holds se aplican antes de exponer resultados.

## Índices MySQL

La migración `013_discovery_search.sql` agrega:

- `FULLTEXT(search_text)`;
- `(latitude, longitude)`;
- `(country, city)`;
- `property_type`;
- triggers para reconstruir `search_text` cuando cambia inventario/discovery metadata.

El diseño evita incorporar Elasticsearch/OpenSearch mientras el volumen pueda resolverse eficientemente con MySQL. Si más adelante se añade un motor externo, debe ser un read model derivado; MySQL seguirá siendo la fuente transaccional de disponibilidad.

## Mapa

MapLibre GL JS se importa sólo cuando el usuario activa split/mapa. Si `FVS_MAP_STYLE_URL` está vacío, FVS usa un estilo raster OpenStreetMap básico. Un proveedor de tiles/estilo propio debe añadir sus orígenes al CSP de Nginx.

Los markers muestran precio y se sincronizan con las cards. El hover de card activa marker; un marker centra/expone la card correspondiente.

## Interacción y accesibilidad

- skeletons durante búsquedas;
- transitions de entrada/salida de resultados y paneles;
- feedback de favorito/carrito;
- toolbar y buscador sticky;
- teclado: `/`, `M`, `C`, flechas/Enter/Escape en autosuggest;
- `prefers-reduced-motion` reduce animación;
- controles de mapa y filtros tienen labels/ARIA;
- `content-visibility:auto` reduce coste de layout en listados largos.

## Performance

- MapLibre lazy-loaded;
- imágenes `loading=lazy`;
- autosuggest debounce 180 ms;
- búsqueda reactiva debounce 320 ms;
- límites server-side (`limit<=240`, `offset<=10000`);
- cards con `content-visibility`;
- un único query contract para lista/mapa/filtros;
- sin build JS y sin runtime Node.
