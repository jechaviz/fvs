#!/bin/sh
set -eu
: "${FVS_PUBLIC_BASE_URL:?FVS_PUBLIC_BASE_URL required}" "${FVS_ADMIN_KEY:?FVS_ADMIN_KEY required}"
curl -fsS --max-time 5 -H "X-Admin-Key: $FVS_ADMIN_KEY" "$FVS_PUBLIC_BASE_URL/api/v1/admin/ops"
printf '\n'
