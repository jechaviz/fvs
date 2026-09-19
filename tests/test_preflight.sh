#!/bin/sh
set -eu
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT HUP INT TERM
: > "$tmp/ca.pem"; mkdir "$tmp/vouchers"
common_env="FVS_ENV=production FVS_DB_HOST=db.example.internal FVS_DB_PORT=3306 FVS_DB_NAME=fvs FVS_DB_USER=fvs_runtime FVS_DB_PASSWORD=0123456789abcdefghijklmnop FVS_DB_CONNECT_TIMEOUT=5 FVS_DB_READ_TIMEOUT=10 FVS_DB_WRITE_TIMEOUT=10 FVS_DB_LOCK_WAIT_TIMEOUT=5 FVS_MIGRATION_LOCK_TIMEOUT=30 FVS_DB_SSL_CA=$tmp/ca.pem FVS_PUBLIC_BASE_URL=https://fvs.example.com FVS_API_BASE_URL=https://fvs.example.com FVS_ALLOWED_ORIGINS=https://fvs.example.com FVS_COOKIE_SECURE=1 FVS_HOLD_SECONDS=900 FVS_CHECKOUT_HOLD_SECONDS=1200 FVS_WORKER_STALE_SECONDS=90 FVS_OUTBOX_LEASE_SECONDS=300 FVS_ADMIN_KEY_SHA256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef FVS_AGENT_KEY_SHA256=abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789 FVS_SCHEMA_REQUIRED=001_current.sql FVS_TERMS_VERSION=2026-09-17 FVS_PAYMENT_PROVIDERS=stripe,mercadopago STRIPE_SECRET_KEY=sk_live_fake STRIPE_WEBHOOK_SECRET=whsec_fake MERCADOPAGO_ACCESS_TOKEN=APP_USR-fake MERCADOPAGO_WEBHOOK_SECRET=fake"
# shellcheck disable=SC2086
env $common_env sh deploy/preflight.sh api true

# Social commerce enabled requires hashed feed auth + HMAC inbound secret.
# shellcheck disable=SC2086
env $common_env FVS_SOCIAL_COMMERCE_ENABLED=1 FVS_SOCIAL_FEED_KEY_SHA256=1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef FVS_SOCIAL_INBOUND_SECRET=abcdefghijklmnopqrstuvwxyz012345 sh deploy/preflight.sh api true
set +e
# shellcheck disable=SC2086
env $common_env FVS_SOCIAL_COMMERCE_ENABLED=1 FVS_SOCIAL_FEED_KEY_SHA256=bad FVS_SOCIAL_INBOUND_SECRET=short sh deploy/preflight.sh api true >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 78 ] || { echo "social commerce preflight accepted weak credentials" >&2; exit 1; }
# shellcheck disable=SC2086
env $common_env FVS_EMAIL_MODE=smtp FVS_SMTP_HOST=smtp.example.internal FVS_SMTP_PORT=587 FVS_SMTP_STARTTLS=1 FVS_SMTP_FROM=reservas@example.com FVS_VOUCHER_DIR="$tmp/vouchers" FVS_SUPPORT_AI_ENABLED=1 OPENAI_API_KEY=sk-proj-offline-preflight-test-key FVS_SUPPORT_AI_MODEL=gpt-5.6-luna sh deploy/preflight.sh worker true
set +e
# shellcheck disable=SC2086
env $common_env FVS_ALLOWED_ORIGINS=http://fvs.example.com sh deploy/preflight.sh api true >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 78 ] || { echo "expected unsafe origin to fail with 78, got $rc" >&2; exit 1; }

# Marketing role requires bounded leases/timeouts, a signing secret, and HTTPS connector URLs.
# shellcheck disable=SC2086
env $common_env FVS_MARKETING_LEASE_SECONDS=180 FVS_MARKETING_HTTP_TIMEOUT=20 FVS_MARKETING_SIGNING_SECRET=0123456789abcdefghijklmnopqrstuvwxyz FVS_MARKETING_CHANNELS=webhook FVS_MARKETING_WEBHOOK_URL=https://marketing.example.internal/publish sh deploy/preflight.sh marketing true
set +e
# shellcheck disable=SC2086
env $common_env FVS_MARKETING_LEASE_SECONDS=180 FVS_MARKETING_HTTP_TIMEOUT=20 FVS_MARKETING_SIGNING_SECRET=0123456789abcdefghijklmnopqrstuvwxyz FVS_MARKETING_CHANNELS=webhook FVS_MARKETING_WEBHOOK_URL=http://marketing.example.internal/publish sh deploy/preflight.sh marketing true >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 78 ] || { echo "marketing preflight accepted insecure connector URL" >&2; exit 1; }

echo "PASS preflight"

# Migration role requires dedicated DDL credentials and advisory-lock timeout.
set +e
env $common_env sh deploy/preflight.sh migrate true >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 78 ] || { echo "migration preflight accepted runtime-only DB credentials" >&2; exit 1; }
# shellcheck disable=SC2086
env $common_env FVS_MIGRATE_DB_USER=fvs_migrate FVS_MIGRATE_DB_PASSWORD=abcdefghijklmnopqrstuvwxyz012345 FVS_MIGRATION_LOCK_TIMEOUT=30 sh deploy/preflight.sh migrate true

# Production must store only a digest of the admin key, never the raw key.
set +e
env $common_env FVS_ADMIN_KEY=plaintext-admin-secret sh deploy/preflight.sh api true >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 78 ] || { echo "production preflight accepted plaintext admin key" >&2; exit 1; }

