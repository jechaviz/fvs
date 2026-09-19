# FVS — Software Design Description

## 1. Propósito

Este documento define el diseño operativo del snapshot actual de FVS y convierte los hallazgos del análisis FODA en requisitos técnicos verificables. No es un historial de versiones: describe el estado objetivo vigente, sus invariantes y los gates que deben impedir regresiones.

## 2. Objetivos de diseño

FVS debe mantener una ruta de compra corta y consistente:

`discovery → carrito → hold → checkout → pago → orden → soporte/retención`

El sistema debe optimizar conversión y operación sin sacrificar las garantías transaccionales del core. Las decisiones priorizan KISS/YAGNI: los controles nuevos deben cerrar riesgos concretos y ser ejecutables en CI.

## 3. Arquitectura vigente

- **Core C17:** autoridad de inventario, holds, pricing, promociones, checkout, confirmación, órdenes, outbox, soporte, social commerce y métricas.
- **MySQL 8.4:** persistencia transaccional InnoDB, locks explícitos, schema actual único en `migrations/001_current.sql`.
- **Shims Python/PHP:** dos adaptadores HTTP sobre el mismo ABI C. Deben mantener paridad de rutas y semántica.
- **Frontend Vue 3 CDN/SFC + UnoCSS runtime:** cliente sin Node en runtime; no es autoridad de precio ni disponibilidad. Helpers browser puros compartidos viven en `frontend/js/ui-runtime.js` para mantener el SFC enfocado en estado/orquestación.
- **Workers:** outbox, voucher/email, soporte IA y marketing con leases/fencing.
- **Edge Nginx:** CSP, rate limits, headers y proxy/FastCGI.
- **Release:** manifest SHA-256, ZIP determinista, CI y release workflow.

## 4. Invariantes no negociables

1. El precio y la disponibilidad autoritativos se calculan en el core.
2. Un pago sólo confirma el intento exacto al que fue ligado: provider, payment id, cart version, amount y currency.
3. Webhooks, outbox y jobs deben ser idempotentes y usar lease/fencing donde aplique.
4. El browser nunca recibe secretos internos ni idempotency keys del proveedor.
5. Toda mutación originada por browser exige Origin permitido; webhooks externos usan autenticación criptográfica propia.
6. Python y PHP deben conservar paridad funcional en superficies públicas críticas.
7. Producción usa cuentas DML/DDL separadas y TLS de base de datos.
8. `RELEASE_MANIFEST.json` debe corresponder exactamente con el árbol comprometido.
9. Dependencias de GitHub Actions externas deben fijarse a commits SHA inmutables.
10. Los archivos hotspot auditados no pueden crecer desde su baseline sin una decisión explícita de arquitectura.
11. El funnel mínimo observable debe conservar eventos `search`, `cart`, `checkout` y `booking`.

## 5. FODA convertido a acciones

### Fortalezas a preservar

El core transaccional, la integración MySQL real, la reconciliación de pagos, el outbox con fencing, los runbooks y el release reproducible son activos del diseño. CI debe seguir cubriéndolos y cualquier refactor debe ser incremental.

### Debilidades corregidas

- Manifest desfasado: corregido y convertido en gate de CI.
- Diferencias SEO Python/PHP: corregidas con cache policy y redirects equivalentes.
- Mutaciones de soporte/atribución antes del Origin gate: corregidas en ambos shims.
- Identificador residual de soporte 4.0: eliminado.
- Release tags sin publicación completa: workflow publica GitHub Release y verifica binding de versión.

### Debilidades en reducción activa

Los hotspots actuales son:

| Ruta | Baseline líneas | Baseline bytes | Acción |
| --- | ---: | ---: | --- |
| `core/src/core.c` | 2232 | 198806 | No crecer; las waves ya extrajeron runtime/science a módulos C. |
| `frontend/components/App.vue` | 291 | 58349 | No crecer; helpers puros viven en `frontend/js/ui-runtime.js`. |
| `frontend/css/base.css` | 40 | 46018 | No crecer; separar por responsabilidad cuando se modifique. |
| `shim-python/app.py` | 417 | 34442 | No crecer; provider recovery ya está extraído. |
| `shim-php/index.php` | 82 | 24683 | No crecer; mantener paridad con módulos equivalentes. |

Estos números no son límites “ideales”; son un **ratchet del estado auditado**. Reducirlos es válido. Aumentarlos requiere primero extraer responsabilidad o justificar explícitamente un nuevo baseline.

### Oportunidades activadas

El producto ya emite telemetría a lo largo de discovery, cart, checkout y booking. El siguiente ciclo de producto debe usar esa misma cadena para medir conversión por búsqueda, colección, canal, promoción y cohortes antes de introducir optimización automática.

El objetivo de closed-loop commerce es:

`evento → atribución → métrica → experimento/decisión → cambio de merchandising/campaña → evento`

La automatización debe permanecer guardada por límites de presupuesto, approvals y stop-loss existentes.

### Amenazas mitigadas

- Supply-chain de GitHub Actions: acciones externas fijadas a SHA y gate automático.
- Crecimiento monolítico: architecture ratchet en CI.
- Cambios sensibles sin revisión visible: CODEOWNERS + checklist de PR.
- Integridad de release: manifest gate + versión ligada al core.

### Amenazas residuales

- `main` requiere protección administrativa con **FVS CI / verify** obligatorio. El conector actual no expone escritura de branch protection; debe configurarse en GitHub.
- CDN frontend sigue siendo una frontera externa. El siguiente hardening de supply-chain es self-host/SRI sin introducir Node en runtime.
- SMTP continúa con semántica at-least-once.
- Admin/agent bearer keys deben quedar detrás de SSO/IAP/VPN en despliegues empresariales.

## 6. Gates automatizados

`make verify` debe ejecutar, además de tests/lint/análisis:

- `make architecture-gate`: impide crecimiento de hotspots.
- `make supply-chain-gate`: exige SHA inmutable en `uses:` externos.
- contratos de funnel y paridad en `tests/test_offline.py`.

La CI mantiene un gate separado de `make manifest-verify` antes de instalar dependencias para detectar inmediatamente un árbol no certificado.

## 7. Estrategia de descomposición

No se hará una reescritura masiva. Cuando una feature toque un hotspot:

1. identificar una responsabilidad coherente;
2. extraerla conservando ABI/contrato;
3. mantener tests antes y después;
4. reducir el baseline del hotspot si el archivo realmente disminuye;
5. no añadir capas que no tengan al menos dos consumidores o una frontera operacional clara.

Orden sugerido de extracción del core: pagos/checkout, discovery, commerce/growth, support/outbox y finalmente utilidades DB compartidas.

## 8. Criterio de aceptación de cambios

Un cambio es promovible cuando:

- manifest, architecture gate y supply-chain gate pasan;
- tests C/sanitizers/Python/PHP pasan;
- análisis estático y lint pasan;
- integración MySQL 8 pasa cuando CI la ejecuta;
- cambios de pagos/schema/ops conservan las invariantes relevantes;
- el release gate de infraestructura pasa antes de tráfico productivo.

## 9. Registro de ejecución

Las acciones realizadas se registran en `log.txt`. Git conserva la historia técnica; `log.txt` resume decisiones y evidencias operativas de esta línea de trabajo.
