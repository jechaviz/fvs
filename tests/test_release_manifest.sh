#!/bin/sh
set -eu
root="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/tree"
printf 'alpha\n' > "$tmp/tree/a.txt"
mkdir -p "$tmp/tree/sub"
printf 'beta\n' > "$tmp/tree/sub/b.txt"
python3 "$root/scripts/release_manifest.py" --root "$tmp/tree" --output RELEASE_MANIFEST.json --version test >/dev/null
python3 "$root/scripts/release_manifest.py" --root "$tmp/tree" --output RELEASE_MANIFEST.json --verify >/dev/null
printf 'changed\n' >> "$tmp/tree/a.txt"
set +e
python3 "$root/scripts/release_manifest.py" --root "$tmp/tree" --output RELEASE_MANIFEST.json --verify >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 2 ] || { echo "expected tampered manifest rc=2 got $rc" >&2; exit 1; }
echo 'PASS release manifest'
