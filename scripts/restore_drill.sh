#!/bin/sh
set -eu
[ "$#" -eq 1 ] || { echo "usage: $0 backup.sql.gz" >&2; exit 64; }
file="$1"
: "${FVS_DB_HOST:?FVS_DB_HOST required}"
user="${FVS_RESTORE_ADMIN_USER:-${FVS_MIGRATE_DB_USER:-}}"
pass="${FVS_RESTORE_ADMIN_PASSWORD:-${FVS_MIGRATE_DB_PASSWORD:-}}"
[ -n "$user" ] && [ -n "$pass" ] || { echo "restore-drill admin credentials missing" >&2; exit 78; }
command -v mysql >/dev/null 2>&1 || { echo "mysql client missing" >&2; exit 69; }

ts="$(date -u +%Y%m%d%H%M%S)"
target="fvs_restore_drill_${ts}_$$"
case "$target" in *[!A-Za-z0-9_]*) echo "invalid drill database name" >&2; exit 65;; esac
export MYSQL_PWD="$pass"
set -- -h "$FVS_DB_HOST" -P "${FVS_DB_PORT:-3306}" -u "$user"
if [ -n "${FVS_DB_SSL_CA:-}" ]; then
  set -- "$@" --ssl-mode=VERIFY_IDENTITY --ssl-ca="$FVS_DB_SSL_CA"
  [ -n "${FVS_DB_SSL_CERT:-}" ] && set -- "$@" --ssl-cert="$FVS_DB_SSL_CERT"
  [ -n "${FVS_DB_SSL_KEY:-}" ] && set -- "$@" --ssl-key="$FVS_DB_SSL_KEY"
fi
cleanup(){ mysql "$@" -e "DROP DATABASE IF EXISTS \`$target\`" >/dev/null 2>&1 || true; }
trap cleanup EXIT HUP INT TERM
mysql "$@" -e "CREATE DATABASE \`$target\` CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci"
FVS_RESTORE_DB_NAME="$target" \
FVS_RESTORE_DB_USER="$user" FVS_RESTORE_DB_PASSWORD="$pass" \
FVS_RESTORE_CONFIRM="RESTORE:$target" FVS_ENV=restore-drill \
  "$(dirname "$0")/restore_mysql.sh" "$file"
mysql -N -B "$@" "$target" -e "SELECT CONCAT('orders=',COUNT(*)) FROM orders; SELECT CONCAT('inventory=',COUNT(*)) FROM inventory_slots;"
printf 'restore drill passed target=%s (dropping test database)\n' "$target"
