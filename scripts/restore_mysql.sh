#!/bin/sh
set -eu
umask 077
[ "$#" -eq 1 ] || { echo "usage: $0 backup.sql.gz" >&2; exit 64; }
file="$1"
: "${FVS_DB_HOST:?FVS_DB_HOST required}"
target="${FVS_RESTORE_DB_NAME:-${FVS_DB_NAME:-fvs}}"
user="${FVS_RESTORE_DB_USER:-${FVS_MIGRATE_DB_USER:-${FVS_DB_USER:-}}}"
pass="${FVS_RESTORE_DB_PASSWORD:-${FVS_MIGRATE_DB_PASSWORD:-${FVS_DB_PASSWORD:-}}}"
[ -n "$user" ] && [ -n "$pass" ] || { echo "restore DB credentials missing" >&2; exit 78; }
[ "${FVS_RESTORE_CONFIRM:-}" = "RESTORE:$target" ] || { echo "set FVS_RESTORE_CONFIRM=RESTORE:$target" >&2; exit 78; }
if [ "${FVS_ENV:-development}" = "production" ] && [ "$target" = "${FVS_DB_NAME:-fvs}" ] && [ "${FVS_ALLOW_INPLACE_RESTORE:-0}" != "1" ]; then
  echo "in-place production restore blocked; restore into a new database or set FVS_ALLOW_INPLACE_RESTORE=1 after maintenance/failover" >&2
  exit 78
fi
[ -f "$file" ] && [ -f "$file.sha256" ] || { echo "backup or checksum missing" >&2; exit 66; }
command -v mysql >/dev/null 2>&1 || { echo "mysql client missing" >&2; exit 69; }
command -v gzip >/dev/null 2>&1 || { echo "gzip missing" >&2; exit 69; }
command -v sha256sum >/dev/null 2>&1 || { echo "sha256sum missing" >&2; exit 69; }

(cd "$(dirname "$file")" && sha256sum -c "$(basename "$file").sha256")
gzip -t "$file"
if [ -f "$file.meta" ]; then
  recorded="$(awk -F= '$1=="sha256"{print $2}' "$file.meta" | head -n1)"
  actual="$(sha256sum "$file" | awk '{print $1}')"
  [ -n "$recorded" ] && [ "$recorded" = "$actual" ] || { echo "metadata checksum mismatch" >&2; exit 65; }
fi

export MYSQL_PWD="$pass"
set -- -h "$FVS_DB_HOST" -P "${FVS_DB_PORT:-3306}" -u "$user"
if [ -n "${FVS_DB_SSL_CA:-}" ]; then
  set -- "$@" --ssl-mode=VERIFY_IDENTITY --ssl-ca="$FVS_DB_SSL_CA"
  [ -n "${FVS_DB_SSL_CERT:-}" ] && set -- "$@" --ssl-cert="$FVS_DB_SSL_CERT"
  [ -n "${FVS_DB_SSL_KEY:-}" ] && set -- "$@" --ssl-key="$FVS_DB_SSL_KEY"
fi

gzip -dc "$file" | mysql "$@" "$target"
required="${FVS_SCHEMA_REQUIRED:-001_current.sql}"
status="$(mysql -N -B "$@" "$target" -e "SELECT IF(COUNT(*)>0 AND SUM(dirty)=0 AND SUM(version='${required}')=1,'READY','NOT_READY') FROM schema_migrations")"
[ "$status" = "READY" ] || { echo "restored schema failed readiness verification" >&2; exit 65; }
# Sanity-check critical tables after restore.
mysql -N -B "$@" "$target" -e "SELECT COUNT(*) FROM inventory_slots; SELECT COUNT(*) FROM carts; SELECT COUNT(*) FROM payment_attempts; SELECT COUNT(*) FROM orders;" >/dev/null
printf 'restore verified target=%s\n' "$target"
