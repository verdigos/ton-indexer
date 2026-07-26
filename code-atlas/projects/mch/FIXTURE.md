# Tutorial fixture registry (lead decision v2, 2026-07-27)

One-spine-for-all was the original intent; authors converged differently and
the result is pedagogically BETTER per chapter. The binding rule is now:
**every chapter that shows engine output NAMES its fixture in prose and links
[[big-picture]] for the thin end-to-end walk.** No silent fixture ambiguity.

| Chapter | Fixture (.lz4 in indexer/tests/traces/) | Production case | Note |
|---|---|---|---|
| 1 big-picture, 3 block-tree | `03RZCW7mu3eJp-FWifVzKsRHafUrqQNu4E6YwZxvXJ0=` | Vesting send message | the SPINE — simple chain, ideal for the thin pass |
| 5 matcher-runtime | `-0M6pWgvSrqN06gIYbqrG2UbMCBKwiSuhR3-mZzRCS0=` | Merged deposit, jetton excess | deliberately richer walk for the deep chapter |
| 7 build-programs | `Lsy5I_gotIvt0EYwoPuqd-1s-IyYNR3Sc0XmM_lWKa8=` | jvault.yaml case | |
| 6 parse-layer, 8 actions | `i-gQwY3n…` (ch6) / `013-itRDy…` (ch8) | claim_reward / Coffee staking claim rewards | ch8 caveat MANDATORY, below |
| 2, 9, 10 | none | — | fine |

CH.8 MANDATORY CAVEAT: in production, `013-itRDy…` is
`coffee_staking_claim_rewards`; mch classifies it as a fallback
`jetton_transfer` (protocol not ported). Real row, fine for teaching columns —
NOT "what the indexer stores for this trace". Feeds ch.9's precise claim: the
twin gate proves C++ == Python-mch (ported specs), NOT C++ == production
event_classifier.

Trap (still true): tests/test_cases/jetton-transfer.yaml contains ONLY failed
transfers; successful jetton_transfer rows come from swap/mint/other traces.

Historical: v1 of this file pinned the stonfi swap trace
(`4PNVGQkZJUWAiiPq3rIekA9O5…`) before most chapters landed; superseded by the
registry above.
