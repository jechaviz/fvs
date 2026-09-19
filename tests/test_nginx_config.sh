#!/bin/sh
set -eu
command -v nginx >/dev/null 2>&1 || { echo 'SKIP nginx config test (nginx unavailable)'; exit 0; }
for src in deploy/nginx/python.conf deploy/nginx/php.conf; do
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT HUP INT TERM
  mkdir -p "$tmp/conf.d" "$tmp/html" "$tmp/logs"
  cp /etc/nginx/fastcgi_params "$tmp/fastcgi_params"
  sed 's/http:\/\/api:8000/http:\/\/127.0.0.1:18000/g; s/fastcgi_pass api:9000/fastcgi_pass 127.0.0.1:19000/g' "$src" > "$tmp/conf.d/default.conf"
  cat > "$tmp/nginx.conf" <<CONF
error_log stderr warn;
pid $tmp/nginx.pid;
events { worker_connections 64; }
http {
  include /etc/nginx/mime.types;
  client_body_temp_path $tmp/client_temp;
  proxy_temp_path $tmp/proxy_temp;
  fastcgi_temp_path $tmp/fastcgi_temp;
  uwsgi_temp_path $tmp/uwsgi_temp;
  scgi_temp_path $tmp/scgi_temp;
  include $tmp/conf.d/*.conf;
}
CONF
  mkdir -p "$tmp/client_temp" "$tmp/proxy_temp" "$tmp/fastcgi_temp" "$tmp/uwsgi_temp" "$tmp/scgi_temp"
  nginx -t -c "$tmp/nginx.conf" -p "$tmp" >/dev/null 2>&1 || { nginx -t -c "$tmp/nginx.conf" -p "$tmp"; exit 1; }
  rm -rf "$tmp"; trap - EXIT HUP INT TERM
done
echo 'PASS nginx syntax'
