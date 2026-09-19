#!/bin/sh
set -eu
root="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
sha=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa

mkdir -p "$tmp/good/.github/workflows"
cat > "$tmp/good/.github/workflows/good.yml" <<EOF
name: good
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@$sha
  reuse:
    uses: owner/repo/.github/workflows/reusable.yml@$sha
EOF
python3 "$root/scripts/supply_chain_gate.py" --root "$tmp/good" >/dev/null

mkdir -p "$tmp/bad-step/.github/workflows"
cat > "$tmp/bad-step/.github/workflows/bad.yml" <<'EOF'
name: bad-step
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v6
EOF
set +e
python3 "$root/scripts/supply_chain_gate.py" --root "$tmp/bad-step" >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 2 ] || { echo "expected unpinned step action to fail, got $rc" >&2; exit 1; }

mkdir -p "$tmp/bad-job/.github/workflows"
cat > "$tmp/bad-job/.github/workflows/bad.yml" <<'EOF'
name: bad-job
jobs:
  reuse:
    uses: owner/repo/.github/workflows/reusable.yml@main
EOF
set +e
python3 "$root/scripts/supply_chain_gate.py" --root "$tmp/bad-job" >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 2 ] || { echo "expected unpinned reusable workflow to fail, got $rc" >&2; exit 1; }

echo 'PASS supply-chain gate'
