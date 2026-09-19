#!/bin/sh
set -eu
umask 077

out_dir="${FVS_BACKUP_DIR:-./backups}"
mkdir -p "$out_dir"
ts="$(date -u +%Y%m%dT%H%M%SZ)"
db="${FVS_DB_NAME:-fvs}"
name="${db}-${ts}.sql.gz"
out="$out_dir/$name"
tmp="$out.tmp"
meta="$out.meta"

cleanup(){ rm -f "$tmp" "$meta.tmp" "$out.sha256.tmp"; }
trap cleanup EXIT HUP INT TERM

: "${FVS_DB_HOST:?FVS_DB_HOST required}" "${FVS_DB_NAME:?FVS_DB_NAME required}"
user="${FVS_BACKUP_DB_USER:-${FVS_DB_USER:-}}"
pass="${FVS_BACKUP_DB_PASSWORD:-${FVS_DB_PASSWORD:-}}"
[ -n "$user" ] && [ -n "$pass" ] || { echo "backup DB credentials missing" >&2; exit 78; }
command -v mysqldump >/dev/null 2>&1 || { echo "mysqldump missing" >&2; exit 69; }
command -v gzip >/dev/null 2>&1 || { echo "gzip missing" >&2; exit 69; }
command -v sha256sum >/dev/null 2>&1 || { echo "sha256sum missing" >&2; exit 69; }

export MYSQL_PWD="$pass"
set -- -h "$FVS_DB_HOST" -P "${FVS_DB_PORT:-3306}" -u "$user"
if [ -n "${FVS_DB_SSL_CA:-}" ]; then
  set -- "$@" --ssl-mode=VERIFY_IDENTITY --ssl-ca="$FVS_DB_SSL_CA"
  [ -n "${FVS_DB_SSL_CERT:-}" ] && set -- "$@" --ssl-cert="$FVS_DB_SSL_CERT"
  [ -n "${FVS_DB_SSL_KEY:-}" ] && set -- "$@" --ssl-key="$FVS_DB_SSL_KEY"
fi

# Logical snapshot: transactionally consistent for InnoDB, includes triggers/events/routines.
mysqldump "$@" \
  --single-transaction --quick --triggers --events --routines --hex-blob \
  --set-gtid-purged=OFF --no-tablespaces --default-character-set=utf8mb4 \
  "$FVS_DB_NAME" | gzip -9 > "$tmp"

gzip -t "$tmp"
[ -s "$tmp" ] || { echo "backup is empty" >&2; exit 74; }
mv "$tmp" "$out"
sha256sum "$out" > "$out.sha256.tmp"
mv "$out.sha256.tmp" "$out.sha256"

cat > "$meta.tmp" <<META
format=fvs-mysql-logical-v1
created_at_utc=$ts
database=$FVS_DB_NAME
schema_required=${FVS_SCHEMA_REQUIRED:-unknown}
mysql_host=$FVS_DB_HOST
mysqldump_version=$(mysqldump --version | tr '\n' ' ')
archive=$(basename "$out")
sha256=$(sha256sum "$out" | awk '{print $1}')
META
mv "$meta.tmp" "$meta"

trap - EXIT HUP INT TERM
printf '%s\n' "$out"
