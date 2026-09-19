#!/bin/sh
set -eu
root="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/tree/sub"
printf 'alpha\n' > "$tmp/tree/a.txt"
printf '#!/bin/sh\necho beta\n' > "$tmp/tree/sub/b.sh"
chmod 755 "$tmp/tree/sub/b.sh"
python3 "$root/scripts/release_manifest.py" --root "$tmp/tree" --output RELEASE_MANIFEST.json --version test >/dev/null
python3 "$root/scripts/package_release.py" --root "$tmp/tree" --output "$tmp/a.zip" >/dev/null
python3 "$root/scripts/package_release.py" --root "$tmp/tree" --output "$tmp/b.zip" >/dev/null
ha="$(sha256sum "$tmp/a.zip" | awk '{print $1}')"; hb="$(sha256sum "$tmp/b.zip" | awk '{print $1}')"
[ "$ha" = "$hb" ] || { echo 'deterministic package hash mismatch' >&2; exit 1; }
unzip -t "$tmp/a.zip" >/dev/null
printf 'PASS deterministic release package\n'
