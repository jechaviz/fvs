#!/bin/sh
set -eu
role="${1:-api}"
shift || true

fail(){ echo "FVS preflight failed: $1" >&2; exit 78; }
require_uint(){
  name="$1"; min="$2"; max="$3"; eval "value=\${$name:-}"
  [ -n "$value" ] || fail "$name missing"
  case "$value" in *[!0-9]*) fail "$name must be an integer";; esac
  [ "$value" -ge "$min" ] 2>/dev/null && [ "$value" -le "$max" ] 2>/dev/null || fail "$name out of range ($min..$max)"
}
require_secret(){
  name="$1"; min="$2"; eval "value=\${$name:-}"
  [ "${#value}" -ge "$min" ] || fail "$name must be >=$min chars"
  case "$value" in *replace*|*REPLACE*) fail "$name still looks like a placeholder";; esac
}
require_runtime_db(){
  [ -n "${FVS_DB_USER:-}" ] || fail "FVS_DB_USER missing"
  require_secret FVS_DB_PASSWORD 24
}

case "$role" in api|worker|marketing|migrate) ;; *) fail "unsupported role: $role";; esac

if [ "${FVS_ENV:-development}" = "production" ]; then
  [ -n "${FVS_DB_HOST:-}" ] || fail "FVS_DB_HOST missing"
  [ -n "${FVS_DB_NAME:-}" ] || fail "FVS_DB_NAME missing"
  require_uint FVS_DB_PORT 1 65535
  require_uint FVS_DB_CONNECT_TIMEOUT 1 60
  require_uint FVS_DB_READ_TIMEOUT 1 300
  require_uint FVS_DB_WRITE_TIMEOUT 1 300
  require_uint FVS_DB_LOCK_WAIT_TIMEOUT 1 120

  if [ "${FVS_ALLOW_INSECURE_DB:-0}" != "1" ]; then
    [ -n "${FVS_DB_SSL_CA:-}" ] || fail "FVS_DB_SSL_CA required in production"
    [ -r "${FVS_DB_SSL_CA}" ] || fail "FVS_DB_SSL_CA is not readable"
  fi
  cert="${FVS_DB_SSL_CERT:-}"; key="${FVS_DB_SSL_KEY:-}"
  if [ -n "$cert" ] || [ -n "$key" ]; then
    [ -n "$cert" ] && [ -n "$key" ] || fail "FVS_DB_SSL_CERT and FVS_DB_SSL_KEY must be configured together"
    [ -r "$cert" ] || fail "FVS_DB_SSL_CERT is not readable"
    [ -r "$key" ] || fail "FVS_DB_SSL_KEY is not readable"
  fi

  if [ "$role" = "migrate" ]; then
    [ -n "${FVS_MIGRATE_DB_USER:-}" ] || fail "FVS_MIGRATE_DB_USER missing; production migrations require a dedicated DDL account"
    require_secret FVS_MIGRATE_DB_PASSWORD 24
    require_uint FVS_MIGRATION_LOCK_TIMEOUT 1 600
  else
    require_runtime_db
    [ -n "${FVS_SCHEMA_REQUIRED:-}" ] || fail "FVS_SCHEMA_REQUIRED missing"
  fi

  if [ "$role" = "api" ]; then
    require_uint FVS_HOLD_SECONDS 60 7200
    require_uint FVS_CHECKOUT_HOLD_SECONDS 60 7200
    require_uint FVS_WORKER_STALE_SECONDS 10 86400

    case "${FVS_PUBLIC_BASE_URL:-}" in https://*) ;; *) fail "FVS_PUBLIC_BASE_URL must use https";; esac
    case "${FVS_API_BASE_URL:-}" in https://*) ;; *) fail "FVS_API_BASE_URL must use https";; esac
    [ "${FVS_COOKIE_SECURE:-0}" = "1" ] || fail "FVS_COOKIE_SECURE=1 required"

    origins="${FVS_ALLOWED_ORIGINS:-}"
    [ -n "$origins" ] || fail "FVS_ALLOWED_ORIGINS missing"
    oldifs="$IFS"; IFS=','
    for origin in $origins; do
      case "$origin" in https://*) ;; *) IFS="$oldifs"; fail "every FVS_ALLOWED_ORIGINS entry must use https";; esac
      case "$origin" in *'*'*|*localhost*|*127.0.0.1*) IFS="$oldifs"; fail "FVS_ALLOWED_ORIGINS contains unsafe production origin";; esac
    done
    IFS="$oldifs"

    adminhash="${FVS_ADMIN_KEY_SHA256:-}"
    [ "${#adminhash}" -eq 64 ] || fail "FVS_ADMIN_KEY_SHA256 must be a 64-character SHA-256 hex digest"
    case "$adminhash" in *[!0-9a-fA-F]*) fail "FVS_ADMIN_KEY_SHA256 must be hexadecimal";; esac
    [ -z "${FVS_ADMIN_KEY:-}" ] || fail "do not store plaintext FVS_ADMIN_KEY on production server; configure FVS_ADMIN_KEY_SHA256"
    [ -n "${FVS_TERMS_VERSION:-}" ] || fail "FVS_TERMS_VERSION missing"
    agenthash="${FVS_AGENT_KEY_SHA256:-}"
    [ "${#agenthash}" -eq 64 ] || fail "FVS_AGENT_KEY_SHA256 must be a 64-character SHA-256 hex digest"
    case "$agenthash" in *[!0-9a-fA-F]*) fail "FVS_AGENT_KEY_SHA256 must be hexadecimal";; esac
    [ -z "${FVS_AGENT_KEY:-}" ] || fail "do not store plaintext FVS_AGENT_KEY on production server"

    if [ "${FVS_SOCIAL_COMMERCE_ENABLED:-0}" = "1" ]; then
      feedhash="${FVS_SOCIAL_FEED_KEY_SHA256:-}"
      [ "${#feedhash}" -eq 64 ] || fail "FVS_SOCIAL_FEED_KEY_SHA256 must be a 64-character SHA-256 hex digest"
      case "$feedhash" in *[!0-9a-fA-F]*) fail "FVS_SOCIAL_FEED_KEY_SHA256 must be hexadecimal";; esac
      require_secret FVS_SOCIAL_INBOUND_SECRET 24
    fi

    providers="${FVS_PAYMENT_PROVIDERS:-stripe,mercadopago}"
    [ -n "$providers" ] || fail "FVS_PAYMENT_PROVIDERS missing"
    oldifs="$IFS"; IFS=','
    for provider in $providers; do
      case "$provider" in
        stripe)
          [ -n "${STRIPE_SECRET_KEY:-}" ] && [ -n "${STRIPE_WEBHOOK_SECRET:-}" ] || { IFS="$oldifs"; fail "Stripe secrets missing"; }
          case "${STRIPE_WEBHOOK_SECRET}" in whsec_*) ;; *) IFS="$oldifs"; fail "STRIPE_WEBHOOK_SECRET has unexpected format";; esac
          ;;
        mercadopago)
          [ -n "${MERCADOPAGO_ACCESS_TOKEN:-}" ] && [ -n "${MERCADOPAGO_WEBHOOK_SECRET:-}" ] || { IFS="$oldifs"; fail "Mercado Pago secrets missing"; }
          ;;
        *) IFS="$oldifs"; fail "unsupported provider in FVS_PAYMENT_PROVIDERS";;
      esac
    done
    IFS="$oldifs"
  fi

  if [ "$role" = "worker" ]; then
    require_uint FVS_OUTBOX_LEASE_SECONDS 30 3600
    [ "${FVS_EMAIL_MODE:-log}" = "smtp" ] || fail "workers require FVS_EMAIL_MODE=smtp"
    [ -n "${FVS_SMTP_HOST:-}" ] || fail "FVS_SMTP_HOST missing"
    [ -n "${FVS_SMTP_FROM:-}" ] || fail "FVS_SMTP_FROM missing"
    [ "${FVS_SMTP_STARTTLS:-1}" = "1" ] || fail "production SMTP requires STARTTLS"
    require_uint FVS_SMTP_PORT 1 65535
    voucher_dir="${FVS_VOUCHER_DIR:-/app/var/vouchers}"
    [ -d "$voucher_dir" ] && [ -w "$voucher_dir" ] || fail "FVS_VOUCHER_DIR must exist and be writable"
    if [ "${FVS_SUPPORT_AI_ENABLED:-1}" = "1" ]; then
      require_secret OPENAI_API_KEY 20
      [ -n "${FVS_SUPPORT_AI_MODEL:-}" ] || fail "FVS_SUPPORT_AI_MODEL missing"
    fi
  fi

  if [ "$role" = "marketing" ]; then
    require_uint FVS_MARKETING_LEASE_SECONDS 30 3600
    require_uint FVS_MARKETING_HTTP_TIMEOUT 1 120
    require_secret FVS_MARKETING_SIGNING_SECRET 24
    channels="${FVS_MARKETING_CHANNELS:-}"
    [ -n "$channels" ] || fail "FVS_MARKETING_CHANNELS missing"
    oldifs="$IFS"; IFS=','
    for channel in $channels; do
      upper=$(printf '%s' "$channel" | tr '[:lower:]-' '[:upper:]_')
      eval "url=\${FVS_MARKETING_${upper}_URL:-}"
      [ -n "$url" ] || { IFS="$oldifs"; fail "FVS_MARKETING_${upper}_URL missing"; }
      case "$url" in https://*) ;; *) IFS="$oldifs"; fail "FVS_MARKETING_${upper}_URL must use https";; esac
    done
    IFS="$oldifs"
  fi
fi

[ "$#" -gt 0 ] || exit 0
exec "$@"
