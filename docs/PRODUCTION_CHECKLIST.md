# Production go-live checklist

## Before first traffic

- Managed/external MySQL 8.4 HA with TLS verification, automated PITR/binlog retention and a tested failover path.
- Separate DML runtime, DDL migration and backup/read accounts. API and worker do not receive DDL credentials.
- `FVS_ADMIN_KEY_SHA256` generated from a >=32-character operator key; raw key is stored only in the operator/secret system, not on the API server.
- Stripe and Mercado Pago production webhook URLs registered and signatures validated in staging.
- SMTP credentials verified and `FVS_EMAIL_MODE=smtp`; voucher storage is durable/shared or replaced with object storage.
- At least two API replicas and two workers across failure domains for HA deployments.
- Edge TLS, WAF/rate limiting and request-size limits configured. Nginx limits are defense in depth, not the only perimeter control.
- `/health/live`, `/health/ready`, `/admin/ops` and `/admin/metrics` integrated into monitoring.
- Alerts from `deploy/observability/prometheus-rules.yml` installed and routed to an on-call destination.

## Every release

1. Verify `RELEASE_MANIFEST.json`; build the distributable with `package_release.py`, then build immutable images once. Record ZIP SHA-256, image digests, manifest tree SHA-256 and source revision.
2. Run `make test analyze lint` and the MySQL integration suite in CI.
3. Produce a fresh logical backup and run `scripts/verify_backup.py`; pass the same backup to `release_gate.py --backup ...`; periodically run `restore_drill.sh`, not only checksum verification.
4. Run migrations once with the DDL account. The migrator serializes concurrent rollout attempts using MySQL `GET_LOCK`.
5. Start the inactive color. Require readiness, worker heartbeat and release gate success.
6. Run smoke/load checks, then canary 1–5%, progressive ramp and full promotion.
7. Keep the previous application color available through the rollback window. Do not reverse an expand-only schema migration just to roll back application traffic.

## Block promotion when

- schema is dirty/missing, readiness fails, or core version is unexpected;
- any outbox dead-letter or stale processing lease exists;
- a worker is stale/missing;
- manual-review volume exceeds the explicitly accepted threshold;
- backup is missing, stale, corrupt or its metadata/checksum does not match;
- payment-provider webhook validation or reconciliation tests are failing.
