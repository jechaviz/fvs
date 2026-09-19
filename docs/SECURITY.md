# Seguridad FVS

## Fronteras de confianza

1. **Browser:** no confiable para importe, disponibilidad ni confirmación.
2. **Shim:** HTTP/cookie/origin/admin auth, firma de webhook y consultas al proveedor.
3. **Core C:** frontera transaccional de reglas de negocio.
4. **MySQL:** locks/persistencia; runtime DML separado de DDL.
5. **Proveedor de pago:** fuente remota de cobro; siempre conciliado contra snapshots FVS.

## Controles principales

- Secreto de carrito aleatorio; sólo hash SHA-256 en MySQL.
- Cookie `HttpOnly`, `SameSite=Lax`, `Secure` obligatorio en producción y path limitado.
- Mutaciones del browser exigen `Origin` exacto permitido.
- Stripe/Mercado Pago: firma + re-fetch remoto + binding de `attempt/version/amount/currency/provider_payment_id`.
- Importes enteros (`*_minor`), sin `float` para conciliación.
- `quote_refreshed` obliga revisión visible si expiró el hold.
- Confirmación atómica multi-propiedad o `manual_review`.
- Webhooks y outbox con idempotencia, lease y fencing token.
- MySQL `FOR UPDATE`, orden estable de locks y clasificación controlada de 1205/1213 como retry.
- Connect/read/write/lock-wait timeouts acotados.
- Migraciones checksum/dirty + advisory lock.
- Cuenta DML runtime separada de cuenta DDL de migración.
- Admin key productivo almacenado únicamente como `FVS_ADMIN_KEY_SHA256`; runtime ignora fallback plaintext cuando `FVS_ENV=production`.
- Nginx: CSP, nosniff, frame deny, permissions policy, límites por endpoint y logs JSON.
- Containers: read-only filesystem, tmpfs acotado, `no-new-privileges`, capabilities eliminadas y PID limits.
- Errores internos SQL/proveedor no se devuelven al cliente.

## CSP / frontend runtime

El requisito de Vue SFC compilado en navegador obliga `script-src 'unsafe-eval'`; UnoCSS runtime obliga `style-src 'unsafe-inline'`. JavaScript inline sigue prohibido. Es un trade-off explícito del requisito “sin Node/build”.

Para un perfil todavía más restrictivo, el siguiente paso sería precompilar/vendorear frontend en CI sin introducir Node en runtime, o espejar dependencias CDN y usar SRI cuando el loader lo permita.

## Admin/backoffice

`X-Admin-Key` es un bootstrap secret de operación, no un sistema IAM completo. En producción ponga `/api/v1/admin/*` detrás de SSO/IAP/VPN/ACL además del hash local. Rote el raw key y actualice sólo su SHA-256 en el servidor.

La reactivación de dead-letter queda auditada en `ops_actions`. No se implementa “resolver manual_review con un clic” porque esa acción puede implicar inventario/cobro/reembolso y requiere un flujo de conciliación explícito.

## Riesgos residuales

- **CDN supply-chain:** dependencias pinneadas, pero externas; para mayor criticidad, self-host.
- **SMTP at-least-once:** caída después de aceptación SMTP y antes del ACK puede duplicar email; use proveedor con idempotencia si se requiere exactly-once práctico.
- **Nginx rate limit:** es defensa en profundidad. El edge/WAF debe aplicar límites globales y resolver correctamente IP del cliente.
- **Integridad de rama:** la CI verifica el manifest comprometido, pero `main` debe protegerse en GitHub con el check **FVS CI / verify** obligatorio para impedir bypass por push directo.
- **Backup confidentiality:** los scripts verifican integridad/restore, pero cifrado y retención deben venir del storage/secret/KMS de la plataforma.
- **Admin key:** aunque el servidor guarda hash, el raw key sigue siendo bearer credential del operador; protéjalo con secret manager y red privada.

## Go-live mínimo

Consulte `docs/PRODUCTION_CHECKLIST.md`. No promover sin MySQL integration tests de staging, restore drill, alertas, webhooks live verificados y un rollback probado.

## Soporte IA y growth

- El modelo de soporte no recibe números de tarjeta/CVV ni debe ejecutar cambios financieros o de reserva.
- `store=false` se usa en Responses API y la KB se carga desde archivo aprobado por despliegue.
- Acciones sensibles fuerzan handoff humano; la autorización final vive en el dominio FVS, no en texto generado.
- La consola humana usa `FVS_AGENT_KEY_SHA256`; para despliegues empresariales grandes se recomienda SSO/OIDC por agente delante del shim.
- Los workers de marketing sólo ejecutan jobs aprobados y reciben guardrails del core. Los tokens de redes viven en el rol `marketing`, no en API/web.
- Los adapters sociales deben estar en HTTPS, con allowlist de egress, tokens de menor privilegio y rotación independiente.
- La atribución first-party usa identificadores anónimos; revise consentimiento/privacidad según jurisdicción antes de habilitar tracking o retargeting.
