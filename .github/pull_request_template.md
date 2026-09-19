## FVS change checklist

- [ ] I ran `make verify`.
- [ ] I regenerated `RELEASE_MANIFEST.json` after the final source change.
- [ ] If DB semantics changed, MySQL 8 integration passed.
- [ ] If checkout/payments changed, provider binding, amount/currency/cart-version and idempotency invariants remain covered.
- [ ] If a browser mutation changed, Origin enforcement remains covered.
- [ ] If Python/PHP HTTP behavior changed, shim parity remains covered.
- [ ] If a hotspot changed, it did not grow past the architecture ratchet; otherwise responsibility was extracted/reviewed.
- [ ] New external GitHub Actions are pinned to immutable commit SHAs.
- [ ] Operational or release behavior changes are reflected in SDD.md/log.txt or the relevant runbook.
