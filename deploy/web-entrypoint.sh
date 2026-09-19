#!/bin/sh
set -eu
key="${STRIPE_PUBLISHABLE_KEY:-}"
if [ -n "$key" ] && ! printf '%s' "$key" | grep -Eq '^pk_[A-Za-z0-9_]+$'; then
  echo "Invalid STRIPE_PUBLISHABLE_KEY format" >&2
  exit 78
fi
umask 077
# Static application assets are baked into the image. Only this tiny runtime
# configuration file is generated, in tmpfs in the production compose files.
attr="${FVS_MARKETING_ATTRIBUTION_ENABLED:-1}"
case "$attr" in 0|1) ;; *) echo "Invalid FVS_MARKETING_ATTRIBUTION_ENABLED" >&2; exit 78;; esac
map_style="${FVS_MAP_STYLE_URL:-}"
if [ -n "$map_style" ]; then
  case "$map_style" in
    https://*|http://*) ;;
    *) echo "Invalid FVS_MAP_STYLE_URL: expected http(s) URL" >&2; exit 78;;
  esac
  if printf '%s' "$map_style" | grep -Eq '[\\"[:cntrl:]]'; then
    echo "Invalid FVS_MAP_STYLE_URL characters" >&2
    exit 78
  fi
fi
printf 'window.FVS_STRIPE_PK = "%s";\nwindow.FVS_MARKETING_ATTRIBUTION_ENABLED = %s;\nwindow.FVS_MAP_STYLE_URL = "%s";\n' \
  "$key" "$([ "$attr" = 1 ] && printf true || printf false)" "$map_style" > /tmp/fvs-config.js
chmod 0444 /tmp/fvs-config.js
exec nginx -g 'daemon off;'
