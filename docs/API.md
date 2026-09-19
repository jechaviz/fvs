# API HTTP FVS

Los shims PHP y Python implementan el mismo contrato bajo `/api/v1`.

## Salud

| Método | Ruta | Uso |
|---|---|---|
| GET | `/health/live` | Proceso/core cargado; no depende de MySQL. |
| GET | `/health/ready` | MySQL usable, migración requerida aplicada y ninguna migración `dirty`. |

Readiness fallida responde `503` + `Retry-After`.

## Inventario

`GET /inventory?check_in=YYYY-MM-DD&check_out=YYYY-MM-DD&guests=N`

Devuelve slots activos/no reservados dentro de la ventana solicitada y con capacidad suficiente.

## Carrito

`POST /cart` crea carrito y envía cookie `fvs_cart_secret` HttpOnly. Respuesta: `cart_id`.

`GET /cart/{cart_id}` devuelve estado, versión, total, moneda y artículos.

`POST /cart/{cart_id}/items`

```json
{"slot_id":"<uuid>","guests":2}
```

`DELETE /cart/{cart_id}/items/{slot_id}` libera el hold si pertenece a ese carrito.

## Checkout

`POST /cart/{cart_id}/checkout`

```json
{"provider":"stripe","email":"cliente@example.com","terms_accepted":true}
```

Estados importantes:

- `quote_refreshed`: expiró un hold; se refrescó cotización y **no** se creó el pago todavía;
- `creating` / `pending`: intento de checkout activo;
- `provider_reconciliation_pending`: resultado remoto ambiguo; reconsultar, no crear otro cobro;
- `paid` / `confirmed`: reserva confirmada;
- `manual_review`: pago requiere conciliación humana y no existe reserva parcial silenciosa.

`GET /cart/{cart_id}/checkout/status` reanuda idempotentemente el mismo intento.

## Webhooks

- `POST /webhooks/stripe`
- `POST /webhooks/mercadopago`

Se autentican por firma y se vuelve a consultar el objeto remoto. Un evento leased por otro request retorna `503 Retry-After: 2`; uno ya completado retorna 200 como duplicado.

## Backoffice / operaciones

Todos los endpoints admin requieren `X-Admin-Key`. En producción el servidor **no** guarda ese valor: compara `SHA-256(raw_key)` con `FVS_ADMIN_KEY_SHA256`.

### Importar inventario

`POST /admin/import`

Headers:

- `X-Admin-Key`
- `X-Filename`

Body: bytes CSV/TSV/XLSX/XLSM.

### Snapshot operacional

`GET /admin/ops`

Incluye `manual_review`, dead-letter/retry/stale de outbox, stale webhooks, holds próximos a expirar y workers activos/stale.

### Métricas Prometheus

`GET /admin/metrics`

`Content-Type: text/plain; version=0.0.4`. Gauges de baja cardinalidad como `fvs_outbox_dead`, `fvs_workers_stale`, `fvs_manual_review_payments` y `fvs_ops_degraded`.

### Pagos en revisión manual

`GET /admin/manual-review?limit=100`

Devuelve intentos recientes con proveedor, payment id ligado, importe, moneda, email, error y timestamp. Evita inspección SQL directa.

### Requeue de dead-letter

`POST /admin/outbox/{event_id}/requeue`

Header opcional `X-Admin-Actor`. Sólo acepta eventos `dead`, los vuelve a `retry` y registra `ops_actions`.

## Errores

Los shims no exponen mensajes SQL ni cuerpos internos de proveedor. Responden códigos estables y `request_id`. Deadlock/lock-wait conocidos retornan error `retry` con HTTP 503 y `Retry-After: 1`.

## Soporte

### Cliente

- `POST /support` crea hilo y cookie HttpOnly de soporte.
- `GET /support/{thread_id}` recupera historial del huésped.
- `POST /support/{thread_id}/messages` agrega mensaje de cliente y, si IA está habilitada, encola `support.ai_requested`.

### Agente humano

Rutas `/agent/*` requieren `X-Agent-Key` y `X-Agent-ID`.

- `POST /agent/presence` actualiza disponibilidad/capacidad.
- `GET /agent/queue` devuelve cola por prioridad/SLA.
- `POST /agent/threads/{id}/claim` reclama el caso.
- `GET /agent/threads/{id}` devuelve contexto completo confiable.
- `POST /agent/threads/{id}/messages` crea respuesta pública o nota interna.
- `POST /agent/threads/{id}/resolve` resuelve el hilo.

## Marketing / growth

Público first-party:

- `POST /marketing/event` registra atribución anónima de funnel.

Admin:

- `GET|POST /admin/marketing/campaigns`
- `POST /admin/marketing/campaigns/{id}/configure`
- `POST /admin/marketing/campaigns/{id}/approve`
- `POST /admin/marketing/campaigns/{id}/pause`
- `POST /admin/marketing/creatives`
- `POST /admin/marketing/creatives/{id}/approve`
- `POST /admin/marketing/jobs`
- `GET /admin/marketing/dashboard?days=N`

Los jobs sólo son claimables por el worker si la campaña/creativo cumple gates y guardrails.

## SEO

- `GET /stay/{slug}` página SSR indexable.
- `GET /sitemap.xml`
- `GET /robots.txt`
- `POST /admin/seo/rebuild`
- `POST /admin/seo/pages`

El frontend SPA no intercepta `/stay/*`; Nginx lo envía al shim para SSR.
