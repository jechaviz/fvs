FROM debian:bookworm-slim AS corebuild
RUN apt-get update && apt-get install -y --no-install-recommends build-essential cmake ninja-build pkg-config libmariadb-dev libssl-dev ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src/core
COPY core/ .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel 1 && ctest --test-dir build --output-on-failure

FROM php:8.4.25-fpm-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends libmariadb3 libssl3 libcurl4-openssl-dev libzip-dev libxml2-dev ca-certificates && docker-php-ext-install -j2 curl zip simplexml opcache pcntl && rm -rf /var/lib/apt/lists/*
COPY --from=corebuild /src/core/build/libfvs_core.so /usr/local/lib/libfvs_core.so
COPY --from=corebuild /src/core/build/fvs_migrate /usr/local/bin/fvs_migrate
RUN ldconfig && printf 'ffi.enable=preload\nffi.preload=/app/shim-php/fvs_ffi_preload.h\n' > /usr/local/etc/php/conf.d/99-fvs-ffi.ini
COPY deploy/php-production.ini /usr/local/etc/php/conf.d/98-fvs-production.ini
WORKDIR /app
COPY shim-php/ /app/shim-php/
COPY workers/ /app/workers/
COPY migrations/ /app/migrations/
COPY deploy/ /app/deploy/
COPY deploy/php-fpm-fvs.conf /usr/local/etc/php-fpm.d/zz-fvs.conf
RUN mkdir -p /app/var/vouchers && chown -R www-data:www-data /app
ENV FVS_CORE_LIB=/usr/local/lib/libfvs_core.so
USER www-data
EXPOSE 9000
CMD ["sh","/app/deploy/preflight.sh","api","php-fpm","-F"]
