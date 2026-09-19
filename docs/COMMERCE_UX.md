# Commerce UX y compresión de decisión

## Objetivo

Reducir saltos entre descubrimiento y compra sin convertir el catálogo en un checkout agresivo. El usuario puede descubrir, comparar, revisar y añadir monetización incremental dentro de una única superficie persistente.

## Pipeline comprimido

1. landing / deep link / colección;
2. discovery omnibox + filtros + mapa;
3. card → quick-view o compare;
4. `Reservar` crea/renueva hold y abre drawer;
5. add-ons + promo/referral + términos en el mismo drawer;
6. pago;
7. orden/voucher/conversión/loyalty.

No hay una página obligatoria de “detalle” entre card y hold. `+ Carrito` conserva el modo multipropiedad para quien aún compara.

## Superficies

- **Collections rail:** escenarios editoriales con filtros/merchandising JSON.
- **Compare tray:** selección persistida localmente, máximo cuatro.
- **Decision modal:** precio, rating, capacidad, recámaras, baños, fechas y amenidades lado a lado.
- **Quick-view:** descripción, facts, amenities, precio y recomendaciones sin abandonar contexto.
- **Booking drawer:** línea de estancias, extras, ahorro, total y pago.
- **Social offer banner:** explica el origen/promoción sin crear holds automáticamente.

## Principios de interacción

- acción explícita antes de hold/origin binding;
- cambios de monetización invalidan/reconstruyen el Payment Element;
- precio siempre viene del core;
- promociones inválidas no se “simulan” client-side;
- `prefers-reduced-motion` desactiva transform/transition intensivos;
- mapa permanece lazy-loaded.

## Commerce state

El frontend consume `commerce/home` para colecciones, extras, planes y promos públicas. El carrito renderiza `subtotal_minor`, `addons_minor`, `discount_minor` y `total_minor` tal como salen de `libfvs_core`.
