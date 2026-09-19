# Backup / restore runbook

## Backup

Use a dedicated SSL-only backup account and run:

```bash
scripts/backup_mysql.sh
```

Cada backup produce:

- `*.sql.gz` — dump lógico transaccional InnoDB;
- `*.sha256` — checksum;
- `*.meta` — formato, timestamp UTC, DB, schema requerido, host y checksum.

Guarde los tres fuera del host de aplicación y cifre en la capa de storage/KMS.

Antes de una migración o restore:

```bash
python3 scripts/verify_backup.py backups/fvs-....sql.gz \
  --max-age-hours 26 \
  --required-schema 008_worker_lifecycle.sql
```

El verifier rechaza checksum roto, gzip truncado, metadata incoherente, schema distinto o backup stale. Una verificación de archivo **no** demuestra restaurabilidad.

## Restore drill

Ejecute periódicamente:

```bash
scripts/restore_drill.sh backups/fvs-....sql.gz
```

El drill crea una DB temporal, restaura, verifica `schema_migrations` y tablas críticas, reporta conteos y elimina la DB de prueba.

## Restore real

`scripts/restore_mysql.sh` requiere `FVS_RESTORE_CONFIRM=RESTORE:<db>`. Un restore in-place sobre la DB productiva se bloquea salvo `FVS_ALLOW_INPLACE_RESTORE=1`; prefiera restaurar en una DB/cluster nuevo y hacer failover controlado.

## Política recomendada

- PITR/binlogs administrados para RPO corto.
- logical backup diario para portabilidad/auditoría.
- restore drill al menos trimestral y antes de cambios de plataforma importantes.
- documentar RPO/RTO **medidos**, no asumidos.
