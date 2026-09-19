#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NAME="fvs-mysql-test-$$"
PORT="${FVS_TEST_MYSQL_PORT:-33306}"
cleanup(){ docker rm -f "$NAME" >/dev/null 2>&1 || true; }
trap cleanup EXIT INT TERM
docker run -d --rm --name "$NAME" -e MYSQL_ROOT_PASSWORD=root -e MYSQL_DATABASE=fvs -e MYSQL_USER=fvs -e MYSQL_PASSWORD=fvs -p "$PORT:3306" mysql:8.4.11 --default-time-zone=+00:00 --transaction-isolation=READ-COMMITTED >/dev/null
for i in $(seq 1 60); do docker exec "$NAME" mysqladmin ping -uroot -proot --silent >/dev/null 2>&1 && break; sleep 1; done
export FVS_DB_HOST=127.0.0.1 FVS_DB_PORT="$PORT" FVS_DB_NAME=fvs FVS_DB_USER=fvs FVS_DB_PASSWORD=fvs FVS_TEST_MYSQL_CONTAINER="$NAME"
"$ROOT/core/build/release/fvs_migrate" "$ROOT/migrations"
FVS_CORE_LIB="$ROOT/core/build/release/libfvs_core.so" python3 "$ROOT/tests/mysql_integration.py"
