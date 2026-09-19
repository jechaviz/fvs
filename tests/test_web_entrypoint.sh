#!/bin/sh
set -eu
# The production entrypoint must never rewrite baked frontend assets.
grep -Fq '/tmp/fvs-config.js' deploy/web-entrypoint.sh
! grep -Fq 'html-src' deploy/Dockerfile.web
! grep -Eq 'cp -a|rm -rf.*/usr/share/nginx/html' deploy/web-entrypoint.sh
# Guard the only value interpolated into JavaScript.
if printf '%s' 'pk_a";alert(1)//' | grep -Eq '^pk_[A-Za-z0-9_]+$'; then
  echo "unsafe Stripe key regex accepted injection payload" >&2; exit 1
fi
printf '%s' 'pk_live_AbCd0123' | grep -Eq '^pk_[A-Za-z0-9_]+$'
# Map style runtime config must be constrained to URL-safe values.
grep -Fq 'FVS_MAP_STYLE_URL' deploy/web-entrypoint.sh
grep -Fq 'expected http(s) URL' deploy/web-entrypoint.sh
for f in deploy/nginx/python.conf deploy/nginx/php.conf; do
  grep -Fq 'location = /js/config.js' "$f"
  grep -Fq 'alias /tmp/fvs-config.js' "$f"
  grep -Fq 'access_log /dev/stdout fvs_json' "$f"
done
echo "PASS immutable web entrypoint/config validation"
