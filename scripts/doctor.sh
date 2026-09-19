#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
fail=0
check_cmd() {
  if command -v "$1" >/dev/null 2>&1; then
    printf 'OK   %-18s %s\n' "$1" "$(command -v "$1")"
  else
    printf 'MISS %-18s required for %s\n' "$1" "$2" >&2
    fail=1
  fi
}
check_cmd make 'automation'
check_cmd cmake 'native build'
check_cmd python3 'tests and tooling'
check_cmd php 'PHP shim tests'
check_cmd docker 'containerized development/integration'
if command -v ninja >/dev/null 2>&1; then
  printf 'OK   %-18s %s\n' ninja "$(command -v ninja)"
else
  printf 'WARN %-18s %s\n' ninja 'CMake preset may require Ninja' >&2
fi
[ -f .env ] || { echo 'WARN .env is missing; run make bootstrap' >&2; }
[ -f migrations/001_current.sql ] || { echo 'MISS migrations/001_current.sql' >&2; fail=1; }
[ -f .github/workflows/ci.yml ] || { echo 'MISS .github/workflows/ci.yml' >&2; fail=1; }
if [ -f .env ] && grep -q '^FVS_ENV=production$' .env; then
  if grep -Eq 'replace|REPLACE|example\.com|sk_test_|pk_test_' .env; then
    echo 'FAIL production .env still contains example/placeholder values' >&2
    fail=1
  fi
fi
exit "$fail"
