SHELL := /bin/bash
CORE := core
JOBS ?= 1
ANALYZE_TIMEOUT ?= 90
VERSION ?= 6.0.0
PACKAGE ?= ../FVS-$(VERSION).zip
BASE_URL ?= http://127.0.0.1:8080


.PHONY: bootstrap doctor dev-python dev-php down build test test-c test-asan test-offline test-php test-prod-static analyze lint verify mysql-test smoke manifest manifest-verify package clean


bootstrap:
	sh scripts/bootstrap.sh

doctor:
	sh scripts/doctor.sh

dev-python:
	docker compose -f docker-compose.python.yml up --build

dev-php:
	docker compose -f docker-compose.php.yml up --build

down:
	-docker compose -f docker-compose.python.yml down
	-docker compose -f docker-compose.php.yml down

build:
	cmake --preset release -S $(CORE)
	cmake --build $(CORE)/build/release --parallel $(JOBS)

test: test-c test-asan test-offline test-php test-prod-static

test-c: build
	ctest --test-dir $(CORE)/build/release --output-on-failure

test-asan:
	cmake --preset asan -S $(CORE)
	cmake --build $(CORE)/build/asan --parallel $(JOBS)
	ctest --test-dir $(CORE)/build/asan --output-on-failure

test-offline: build
	FVS_CORE_LIB=$(CURDIR)/$(CORE)/build/release/libfvs_core.so python3 tests/test_offline.py

test-php: build
	FVS_CORE_LIB=$(CURDIR)/$(CORE)/build/release/libfvs_core.so php -d ffi.enable=true tests/test_php.php

test-prod-static:
	FVS_ENV=development sh deploy/preflight.sh api true
	sh tests/test_preflight.sh
	sh tests/test_web_entrypoint.sh
	sh tests/test_nginx_config.sh
	sh tests/test_backup_verify.sh
	sh tests/test_release_manifest.sh
	sh tests/test_release_gate.sh
	sh tests/test_package_release.sh
	@set +e; FVS_ENV=production sh deploy/preflight.sh api true >/tmp/fvs-preflight-test.log 2>&1; rc=$$?; set -e; \
		test $$rc -eq 78 || { cat /tmp/fvs-preflight-test.log; echo "expected production preflight rc=78, got $$rc" >&2; exit 1; }
	python3 -m py_compile deploy/gunicorn.conf.py scripts/load_smoke.py scripts/release_gate.py scripts/verify_backup.py scripts/admin_key_hash.py scripts/release_manifest.py scripts/package_release.py
	@for f in deploy/*.sh scripts/*.sh; do sh -n $$f || exit 1; done
	@grep -q 'FVS_SCHEMA_REQUIRED' .env.example
	@grep -q 'FVS_DB_SSL_CA' .env.example
	@grep -q 'FVS_ADMIN_KEY_SHA256' .env.example
	@grep -q 'alias /tmp/fvs-config.js' deploy/nginx/python.conf
	@grep -q 'alias /tmp/fvs-config.js' deploy/nginx/php.conf
	@grep -q 'limit_req_zone' deploy/nginx/python.conf
	@grep -q 'limit_req_zone' deploy/nginx/php.conf
	@grep -q 'opcache.enable=1' deploy/php-production.ini
	@grep -q 'FFI_SCOPE "FVS"' shim-php/fvs_ffi_preload.h
	@grep -q 'proxy_pass http://api:8000' deploy/nginx/python.conf
	@grep -q 'fastcgi_pass api:9000' deploy/nginx/php.conf

analyze:
	@mkdir -p /tmp/fvs-analyze
	timeout $(ANALYZE_TIMEOUT)s gcc -std=c17 -Wall -Wextra -Wpedantic -fanalyzer $$(pkg-config --cflags mariadb 2>/dev/null || pkg-config --cflags libmariadb) -I$(CORE)/include -c $(CORE)/src/core.c -o /tmp/fvs-analyze/core.o
	@if command -v clang >/dev/null 2>&1; then if command -v timeout >/dev/null 2>&1; then timeout 90s clang --analyze -std=c17 $$(pkg-config --cflags mariadb 2>/dev/null || pkg-config --cflags libmariadb) -I$(CORE)/include $(CORE)/src/core.c; else clang --analyze -std=c17 $$(pkg-config --cflags mariadb 2>/dev/null || pkg-config --cflags libmariadb) -I$(CORE)/include $(CORE)/src/core.c; fi; fi

lint:
	python3 -m compileall -q shim-python workers scripts tests deploy/gunicorn.conf.py
	@for f in shim-php/*.php workers/*.php tests/test_php.php; do php -l $$f >/dev/null || exit 1; done
	@for f in deploy/*.sh scripts/*.sh; do sh -n $$f || exit 1; done
	@if command -v node >/dev/null 2>&1; then for f in frontend/js/*.js; do node --check $$f >/dev/null || exit 1; done; fi

verify: test lint analyze

mysql-test: build
	FVS_CORE_LIB=$(CURDIR)/$(CORE)/build/release/libfvs_core.so python3 tests/mysql_integration.py

smoke:
	python3 scripts/load_smoke.py --base $(BASE_URL)

manifest:
	python3 scripts/release_manifest.py --root . --output RELEASE_MANIFEST.json --version $(VERSION)

manifest-verify:
	python3 scripts/release_manifest.py --root . --output RELEASE_MANIFEST.json --verify

package: manifest manifest-verify
	python3 scripts/package_release.py --root . --output $(PACKAGE)

clean:
	rm -rf $(CORE)/build shim-python/__pycache__ workers/__pycache__ scripts/__pycache__ tests/__pycache__ deploy/__pycache__
	find . -name '*.plist' -delete
