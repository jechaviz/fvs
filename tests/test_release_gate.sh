#!/bin/sh
set -eu
root="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
set +e
python3 "$root/scripts/release_gate.py" http://127.0.0.1:1 --admin-key test --samples 1 --backup "$tmp/missing.sql.gz" >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 2 ] || { echo "expected backup gate failure rc=2 got $rc" >&2; exit 1; }
echo 'PASS release gate backup precondition'
