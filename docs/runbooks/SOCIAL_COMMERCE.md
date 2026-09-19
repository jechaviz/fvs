# Runbook — Social commerce

## Activación

1. Configurar `FVS_SOCIAL_COMMERCE_ENABLED=1`.
2. Generar una clave de feed larga; guardar sólo su SHA-256 en `FVS_SOCIAL_FEED_KEY_SHA256`.
3. Generar `FVS_SOCIAL_INBOUND_SECRET` para firmas HMAC de leads.
4. Configurar adapters HTTPS del worker de marketing únicamente para los canales habilitados.
5. Ejecutar `deploy/preflight.sh api true` y `deploy/preflight.sh marketing true`.

## Catálogo

El adapter consulta `GET /api/v1/social/catalog/<channel>` con `X-Social-Feed-Key`. El feed expone sólo inventario activo/no reservado en ese momento. Programar `sync_catalog` periódicamente según los límites del proveedor.

## Links shoppable

Crear con `POST /api/v1/admin/social/links`. Puede apuntar a slot o collection y llevar campaign/creative/promo. El redirect `/s/<token>` incrementa clicks y produce query params de atribución. El carrito vincula el origen sólo cuando el usuario realiza una acción que crea/restaura carrito.

## Conversiones

Una reserva confirmada con `source_campaign_id + source_channel` crea `marketing_jobs.action=send_conversion`. El adapter debe devolver id/provider_ref y deduplicar por `job_id`/`order_id`.

## Leads

`POST /api/v1/social/inbound` exige `X-FVS-Signature: sha256=<HMAC(raw_body)>`. El lead se normaliza y crea un thread de soporte; WhatsApp conserva channel `whatsapp`, el resto entra como `social` para soporte.

## Guardrails

Nunca saltar approval gates. `marketing_job_claim` aplica budget/daily/max daily/stop-loss/target ROAS antes de entregar acciones que aumentan exposición/gasto. `send_conversion` y `sync_metrics` pueden ejecutarse aunque la campaña haya sido pausada, para no perder conciliación.

## Abandono

`recover_abandonment` es un job soportado, pero el operador/CRM debe garantizar consentimiento e identidad adecuados. No habilitar outreach individual sólo porque exista un email de checkout.

## Incidente

- pausar campaña en FVS y en provider si hay drift;
- detener worker de marketing si el adapter devuelve resultados inconsistentes;
- conservar jobs en retry/dead-letter para análisis;
- verificar `manual_review` si hay pago exitoso y conflicto de entitlement;
- no editar contadores de promo/referral manualmente sin reconciliar órdenes/payment attempts.
