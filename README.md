# FVS

FVS es una plataforma transaccional para descubrimiento, reserva y venta de estancias vacacionales. Integra catálogo geográfico, carrito multipropiedad, pagos, promociones, referrals, loyalty, soporte IA/humano, SEO programático y automatización de campañas/social commerce sobre el mismo inventario y pricing.

**Release actual:** `6.0.0`  
**Schema instalable actual:** `migrations/001_current.sql`

## Stack

- Core transaccional: C17 (`libfvs_core`).
- Base de datos: MySQL 8.4 / InnoDB, UTC y `READ COMMITTED`.
- HTTP: shim Python 3.13 o PHP 8.4 sobre el mismo ABI C.
- Frontend: Vue 3 CDN/SFC + UnoCSS runtime; no requiere Node en producción.
- Pagos: Stripe PaymentIntents y Mercado Pago Checkout Pro.
- Operación: outbox/workers con lease, fencing, retry y dead-letter.
- Edge: Nginx con CSP, rate limits y logs estructurados.

## Capacidades principales

El flujo de compra comprime `buscar → comparar → reservar/agregar al carrito → hold → extras/promoción → pago → orden`. El precio se calcula exclusivamente en el core y se fotografía en carrito, intento de pago y orden para evitar divergencias entre UI, proveedor y conciliación.

Discovery incluye omnibox, autosuggest, full-text/ranking, facetas, filtros geográficos/comerciales y mapa lazy-loaded. Commerce incorpora promociones, referrals, add-ons, colecciones, membresías/loyalty y deep links sociales. El origen de campaña puede persistir hasta la orden y disparar conversión/atribución. El soporte combina IA con handoff humano para operaciones sensibles.

## Arranque local

Requisitos recomendados: Docker con Compose, `make`, Python 3, CMake/Ninja y toolchain C para ejecutar toda la validación local.

```bash
make bootstrap
make dev-python
# alternativa:
make dev-php
```

Superficies por defecto:

- catálogo: `http://localhost:8080`
- backoffice: `http://localhost:8080/admin.html`
- consola de agentes: `http://localhost:8080/support-agent.html`

Para detener el entorno:

```bash
make down
```

`make bootstrap` crea `.env` desde `.env.example` sólo si no existe. Antes de usar proveedores externos o producción, sustituye todos los secretos y URLs de ejemplo.

## Validación

El mismo conjunto de gates se usa localmente y en GitHub Actions:

```bash
make doctor       # dependencias y configuración básica
make test         # C, sanitizers, contratos Python/PHP y checks productivos estáticos
make lint         # Python, PHP, shell y JS cuando Node está disponible
make analyze      # analizadores estáticos C
make verify       # test + lint + analyze
make mysql-test   # integración contra una instancia MySQL ya disponible
```

Para ejecutar la integración con MySQL 8.4 aislado mediante Docker:

```bash
./scripts/test_mysql_docker.sh
```

## Release reproducible

```bash
make manifest
make package
```

Esto genera/verifica `RELEASE_MANIFEST.json` y construye un ZIP determinista fuera del árbol del repo. El número de release se controla con `VERSION`, por ejemplo:

```bash
make package VERSION=6.0.0 PACKAGE=../FVS-6.0.0.zip
```

Antes de promover producción, usa backup verificable y el gate blue/green:

```bash
python3 scripts/verify_backup.py backups/fvs-....sql.gz \
  --required-schema 001_current.sql

python3 scripts/release_gate.py https://fvs.example.com \
  --admin-key "$FVS_OPERATOR_ADMIN_KEY" \
  --backup backups/fvs-....sql.gz \
  --required-schema 001_current.sql \
  --expected-core-prefix 6.0.0
```

## Documentación

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md): invariantes, concurrencia, pagos y workers.
- [`docs/API.md`](docs/API.md): contrato HTTP.
- [`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md): despliegue Python/PHP, migración, probes y restore.
- [`docs/AUTOMATION.md`](docs/AUTOMATION.md): comandos, CI/CD, gates y operación accionable.
- [`docs/SECURITY.md`](docs/SECURITY.md): fronteras de confianza y requisitos de go-live.
- [`docs/PRODUCTION_CHECKLIST.md`](docs/PRODUCTION_CHECKLIST.md): checklist de promoción.
- [`docs/SLO.md`](docs/SLO.md): marco de SLO/observabilidad.
- [`docs/DISCOVERY_UX.md`](docs/DISCOVERY_UX.md) y [`docs/COMMERCE_UX.md`](docs/COMMERCE_UX.md): UX actual.
- [`docs/ENTERPRISE_GROWTH.md`](docs/ENTERPRISE_GROWTH.md): soporte, SEO, campañas y atribución.
- [`docs/runbooks/`](docs/runbooks): procedimientos operativos concretos.

## Regla del repositorio

Este repositorio representa **el estado actual del producto**, no el historial de releases anteriores. No se conservan changelogs, diffs de oleadas ni migraciones incrementales antiguas dentro del snapshot actual. La historia de cambios vive en Git; el árbol de trabajo contiene únicamente lo necesario para construir, validar, desplegar y operar la release vigente.
