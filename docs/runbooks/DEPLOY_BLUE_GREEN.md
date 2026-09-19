# Blue/green deployment runbook

1. Build API/worker/web **una sola vez** desde el mismo commit; registre source revision e image digests.
2. Genere backup y ejecute `scripts/verify_backup.py`. No migre con backup stale/corrupto.
3. Ejecute `fvs_migrate` con la cuenta DDL dedicada. El migrador usa checksum/dirty y MySQL advisory lock para impedir dos migraciones simultáneas.
4. Arranque el color inactivo sin tráfico. Todas las replicas deben pasar `/health/live` y `/health/ready`.
5. Espere al heartbeat de workers y ejecute `scripts/release_gate.py` con el raw admin key del operador. El servidor sólo almacena su SHA-256.
6. Ejecute `scripts/load_smoke.py` y un checkout sintético/staging que recorra webhook → order → outbox.
7. Desplace 1–5% del tráfico. Observe edge 5xx/429/p95, proveedores, `/admin/metrics`, `manual_review`, dead-letter y workers durante al menos un ciclo normal de checkout.
8. Aumente progresivamente a 25/50/100% sólo si SLOs y gate permanecen verdes.
9. Mantenga el color previo vivo durante la ventana de rollback.
10. Si hay regresión, revierta **tráfico de aplicación**. No revierta una migración expand-only aplicada salvo que exista reverse migration probada; la versión anterior debe tolerar el schema expandido.

Los Compose productivos incluidos son referencia single-host. HA real requiere edge/LB externo, múltiples replicas y MySQL HA/managed.
