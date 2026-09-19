# Production SLO framework

These are deployment targets, not guarantees. Calibrate them with real traffic and provider behavior before making contractual commitments.

| Signal | Initial target | Measurement |
|---|---:|---|
| Public API availability | 99.9% monthly | non-maintenance requests excluding caller 4xx |
| Checkout API p95 | < 750 ms excluding payment-provider round trips | edge/APM |
| Inventory search p95 | < 400 ms | edge/APM |
| Confirmed-payment to order confirmation | p95 < 60 s | payment webhook timestamp → order confirmation |
| Voucher/email outbox | p95 < 120 s | outbox created → ack |
| RPO | <= 15 min | managed MySQL PITR/binlog + logical backup policy |
| RTO | <= 60 min | quarterly restore/failover drill |

Error-budget policy: stop feature promotion when availability budget is exhausted, dead-letter events are unresolved, workers are stale, or manual-review volume grows unexpectedly. Payment-provider outages should be tracked separately from FVS internal availability while still being visible in the checkout SLO.
