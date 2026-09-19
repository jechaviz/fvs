# FVS observability

`GET /api/v1/admin/metrics` exposes low-cardinality Prometheus text metrics derived from the same transactional operations snapshot as `/admin/ops`. It requires the raw `X-Admin-Key`; the API stores only `FVS_ADMIN_KEY_SHA256` in production.

Do not expose the admin metrics endpoint publicly just because it is scrapeable. Prefer an internal/private route or an identity-aware proxy. If the monitoring system cannot attach `X-Admin-Key`, inject it at a private reverse proxy rather than removing API authentication.

`prometheus-rules.yml` contains starting alert rules. Pair these with edge/load-balancer metrics for request rate, 5xx, 429, latency and TLS health; FVS intentionally does not implement a full metrics server inside the C core.
