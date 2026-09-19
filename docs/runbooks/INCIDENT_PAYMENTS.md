# Payment / reservation incident runbook

- First inspect `/api/v1/admin/ops`; record request IDs and the affected `attempt_id`, provider and `provider_payment_id`.
- If provider payment succeeded but FVS is `manual_review`, do not create inventory manually until provider amount/currency/object ownership are independently verified.
- Never replay a webhook by changing DB rows directly. Allow the webhook lease to expire or replay from the provider; fencing prevents an old worker from completing after a new lease is acquired.
- A dead outbox notification can be requeued with `POST /api/v1/admin/outbox/{event_id}/requeue` plus `X-Admin-Key` and `X-Admin-Actor`. The action is recorded in `ops_actions`.
- If MySQL returns `retry`, clients/edge may retry after the advertised `Retry-After`; never blind-retry an unknown provider request that lacks FVS idempotency metadata.
- Escalate if `manual_review` grows, payment mismatch repeats, or one slot appears booked without an order. Preserve logs and DB snapshot before corrective writes.
