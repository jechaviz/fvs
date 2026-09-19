#!/bin/sh
set -eu
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT HUP INT TERM
b="$tmp/fvs-$(date -u +%Y%m%dT%H%M%SZ).sql.gz"
printf 'CREATE TABLE t(id INT);\n' | gzip -9 > "$b"
sha="$(sha256sum "$b" | awk '{print $1}')"
printf '%s  %s\n' "$sha" "$(basename "$b")" > "$b.sha256"
cat > "$b.meta" <<META
format=fvs-mysql-logical-v1
created_at_utc=$(date -u +%Y%m%dT%H%M%SZ)
database=fvs
schema_required=001_current.sql
mysql_host=db.example
archive=$(basename "$b")
sha256=$sha
META
python3 scripts/verify_backup.py "$b" --max-age-hours 1 --required-schema 001_current.sql >/dev/null
# Corrupt the payload and ensure verification fails.
printf 'x' >> "$b"
set +e
python3 scripts/verify_backup.py "$b" --max-age-hours 1 --required-schema 001_current.sql >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 2 ] || { echo "corrupt backup unexpectedly verified" >&2; exit 1; }
echo "PASS backup verification"
