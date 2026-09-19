# Despliegue FVS

## Elegir shim

### Python

`docker-compose.python.yml` para desarrollo; `deploy/compose.production.python.yml` como referencia productiva. Gunicorn usa `gthread` y cada thread obtiene contexto C/MySQL aislado.

### PHP

`docker-compose.php.yml` para desarrollo; `deploy/compose.production.php.yml` en producción. PHP-FPM usa FFI preload, OPcache y pool acotado; el worker incluye `pcntl` para shutdown limpio.

Ambos llaman la misma `libfvs_core.so` y comparten esquema/semántica.

## Cuentas MySQL

Producción debe separar:

- `FVS_DB_USER/PASSWORD`: DML runtime para API/worker;
- `FVS_MIGRATE_DB_USER/PASSWORD`: DDL, usado sólo por migrator;
- `FVS_BACKUP_DB_USER/PASSWORD`: lectura/backup;
- credencial admin de restore sólo para drills/DR.

`deploy/mysql/production_grants.sql.example` sirve de punto de partida; ajuste privilegios a su proveedor administrado.

## Preflight fail-closed

`deploy/preflight.sh` valida por rol. En `FVS_ENV=production` exige TLS MySQL salvo override explícito, timeouts acotados, secretos fuertes, HTTPS/cookie secure en API, claves de proveedor sólo en API y SMTP TLS sólo en worker. Migrator exige la cuenta DDL dedicada.

El servidor productivo recibe sólo `FVS_ADMIN_KEY_SHA256`. Genérelo con `scripts/admin_key_hash.py` y conserve el raw key sólo en el secret manager/cliente operacional.

## Migraciones

`fvs_migrate /app/migrations` usa:

- checksum SHA-256 por archivo;
- marker `dirty`;
- TLS/timeouts de DB;
- advisory lock MySQL `fvs_schema_migrate` para serializar despliegues concurrentes.

No modifique migraciones ya aplicadas. Use expand/contract para mantener compatibilidad durante blue-green.

## Probes y promoción

- `/api/v1/health/live`: core cargado, sin depender de DB.
- `/api/v1/health/ready`: DB + schema exacto/limpio.
- `/api/v1/admin/ops`: estado operacional.
- `/api/v1/admin/metrics`: scrape Prometheus autenticado.

Antes de promover una coloración:

```bash
python3 scripts/release_gate.py https://fvs.example.com \
  --admin-key "$FVS_OPERATOR_ADMIN_KEY" \
  --expected-core-prefix 6.0.0
```

El gate rechaza core inesperado, readiness falsa, workers faltantes/stale, dead-letter, leases stale, manual-review fuera de tolerancia o métricas degradadas.

## Contenedores

Los Compose productivos configuran filesystem `read_only`, tmpfs explícitos, `no-new-privileges`, `cap_drop=ALL` y límites PID. El frontend está baked en la imagen; sólo `/tmp/fvs-config.js` es generado al iniciar.

Las imágenes de referencia están pinneadas a líneas concretas. En infraestructura real registre y despliegue **digests** de imagen, no sólo tags.

## Backups y restore

Backup lógico:

```bash
scripts/backup_mysql.sh
python3 scripts/verify_backup.py backups/fvs-....sql.gz \
  --required-schema 001_current.sql
```

El verificador revisa SHA-256, gzip completo, metadata, timestamp y schema esperado. Esto **no sustituye** un restore drill.

```bash
scripts/restore_drill.sh backups/fvs-....sql.gz
```

El restore productivo in-place está bloqueado por defecto. Consulte `docs/runbooks/BACKUP_RESTORE.md`.

## Escala horizontal

API es stateless respecto del proceso y puede replicarse. Los workers escalan gracias a `SKIP LOCKED` + lease/fencing. Para HA, use al menos dos API y dos workers distribuidos en failure domains; el Compose incluido es sólo referencia single-host.

Voucher storage debe ser compartido/durable al escalar workers entre hosts, o sustituirse por object storage.
