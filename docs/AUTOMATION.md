# Automatización operativa

Esta guía convierte las tareas frecuentes de FVS en comandos reproducibles. La regla es simple: el comando local y el gate de CI deben ejecutar la misma validación siempre que sea posible.

## Flujo diario

### 1. Preparar entorno

```bash
make bootstrap
make doctor
```

`bootstrap` crea `.env` sólo si falta. `doctor` comprueba herramientas esenciales, archivos críticos y que no queden placeholders peligrosos si `FVS_ENV=production`.

### 2. Levantar aplicación

Python:

```bash
make dev-python
```

PHP:

```bash
make dev-php
```

Detener ambos perfiles:

```bash
make down
```

### 3. Verificar antes de commit/push

```bash
make verify
```

`verify` encadena tests, lint y análisis estático. Para validar MySQL real:

```bash
./scripts/test_mysql_docker.sh
```

## Base de datos

El snapshot actual tiene un único baseline instalable:

```text
migrations/001_current.sql
```

El migrator sigue usando `schema_migrations`, checksum SHA-256, `dirty` marker y advisory lock. El archivo actual no es una secuencia de upgrades históricos: describe el schema que una instalación nueva debe alcanzar.

En producción usa una cuenta DDL separada (`FVS_MIGRATE_DB_*`) y ejecuta el migrator antes de poner tráfico sobre una instancia nueva.

## Manifest y paquete determinista

Generar manifest:

```bash
make manifest VERSION=6.0.0
```

Verificar que el árbol no cambió:

```bash
make manifest-verify
```

Crear paquete:

```bash
make package VERSION=6.0.0 PACKAGE=../FVS-6.0.0.zip
```

El paquete falla si el manifest no coincide con los bytes actuales.

## Smoke test

Con una instancia levantada:

```bash
make smoke BASE_URL=http://127.0.0.1:8080
```

El smoke ejerce readiness en concurrencia y falla si excede el error rate o p95 configurado por `scripts/load_smoke.py`.

## Promoción blue/green

1. Generar y verificar un backup reciente.
2. Desplegar el color inactivo.
3. Ejecutar migración con la cuenta DDL.
4. Esperar readiness.
5. Ejecutar `release_gate.py` contra el color inactivo.
6. Mover tráfico.
7. Observar SLO/errores y conservar el color anterior durante la ventana de rollback.

Ejemplo:

```bash
python3 scripts/release_gate.py https://green.fvs.example.com \
  --admin-key "$FVS_OPERATOR_ADMIN_KEY" \
  --backup "$BACKUP" \
  --required-schema 001_current.sql \
  --expected-core-prefix 6.0.0 \
  --samples 5
```

El gate comprueba liveness/readiness, versión del core, workers, dead-letter, leases stale, manual review y métricas operativas.

## GitHub Actions

`.github/workflows/ci.yml` ejecuta build, sanitizers, contratos de shims, checks productivos, analizadores, lint e integración MySQL en cada push/PR.

`.github/workflows/release.yml` permite generar un artefacto reproducible manualmente o al crear un tag `v*`. La release no sustituye el gate contra infraestructura real; sólo certifica el árbol de código y crea el paquete.

## Tareas de recuperación

Los procedimientos destructivos no se esconden detrás de automatización silenciosa. Usa los runbooks explícitos:

- `docs/runbooks/BACKUP_RESTORE.md`
- `docs/runbooks/DEPLOY_BLUE_GREEN.md`
- `docs/runbooks/INCIDENT_PAYMENTS.md`
- `docs/runbooks/GROWTH_SUPPORT.md`
- `docs/runbooks/SOCIAL_COMMERCE.md`

Para restore productivo, `FVS_ALLOW_INPLACE_RESTORE=1` debe ser una decisión explícita y temporal; por defecto el tooling lo bloquea.
