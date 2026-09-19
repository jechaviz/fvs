# Operación en producción

## Base operativa

- MySQL 8.4/InnoDB HA externo/gestionado, TLS con verificación, UTC y `READ COMMITTED`.
- Cuenta runtime DML, cuenta migración DDL y cuenta backup separadas.
- Dos o más API stateless; PHP-FPM o Python/Gunicorn son shims equivalentes sobre la misma ABI C.
- Dos o más workers cuando se requiera HA; outbox/webhook usan lease + fencing.
- TLS termina en edge/LB/WAF confiable; Nginx interno aplica defensa en profundidad y logging JSON.
- Voucher storage compartido/durable u object storage al escalar hosts.

## Readiness / liveness

`/health/live` prueba proceso/core sin MySQL. `/health/ready` exige MySQL y `schema_migrations` limpio con `FVS_SCHEMA_REQUIRED`. Un rollout con schema ausente/dirty queda fuera de tráfico.

## Fault handling

MySQL 1205 y 1213 se traducen a ABI `retry` / HTTP 503. FVS no reejecuta automáticamente escrituras ambiguas; deja el retry al cliente/edge cuando el resultado es conocido como rollback transitorio.

## Observabilidad

- `/admin/ops`: snapshot operacional JSON.
- `/admin/metrics`: Prometheus text format.
- worker heartbeat/lifecycle.
- Nginx access logs JSON a stdout.
- `deploy/observability/prometheus-rules.yml`: reglas de alerta iniciales.

Alertar como mínimo: readiness, 5xx/429/latencia en edge, dead-letter, worker stale, webhook stale, crecimiento manual-review y errores/latencia de proveedores.

## Secretos

`deploy/preflight.sh` falla cerrado por rol. El API productivo sólo acepta `FVS_ADMIN_KEY_SHA256`; no almacene `FVS_ADMIN_KEY` raw en el servidor. Mantenga secretos en secret manager de plataforma y restrinja `/admin/*` con SSO/IAP/VPN además de la clave.

## Release

Siga `docs/runbooks/DEPLOY_BLUE_GREEN.md` y `docs/PRODUCTION_CHECKLIST.md`. Antes de migrar, valide backup. Antes de promover, ejecute `scripts/release_gate.py`.
