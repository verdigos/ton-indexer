# Frozen action goldens

`actions.golden` is the committed expectation for

```
ton-mch-engine --actions -T indexer/tests/traces
```

run from the repo root (no `-I`: the matcher table is compiled in since EM4).
**165 fixtures, 787 ACTION rows** — one per serializable block on each trace's
post-pass spine, plus 6 synthetic `unknown` rows (the fixtures whose spine
serializes to nothing at all). Rows are printed **sorted** within a fixture,
because spine order is not a stable property (see below).

## Why it exists

It replaces the live Python twin (`twins/py_actions.py`) as the reference for
the fixture actions gate. The twin imports the real Python classifier, so it
dies with it — and Python's `--emulated-traces` path is being retired. This file
is the last snapshot taken while both engines could still be compared.

## Provenance

Re-baselined 2026-07-30 (**277 → 787 rows**) when the row set was moved from the
matcher FIRE list to the `serialize_blocks` SPINE walk, which is what Python
actually does (`block_tree_serializer.py:1641-1681`). Per-row account of the
change, over 143 of the 165 fixtures:

- **+497 `call_contract`, +82 `contract_deploy`, +38 `ton_transfer`** — the basic
  rows for every leaf no matcher consumed. Python has always emitted these; the
  C++ dump only ever rendered matcher products.
- **-58 `jetton_transfer`, -5 `jetton_mint`** — products a LATER matcher absorbed.
  They are off the spine, so Python emits no row of their own for them (the row
  belongs to the outer composite); the fire list still had them.
- **-44 `unknown`** (50 → 6) — those fixtures were never rowless, only
  product-less. `unknown` now fires on the same condition Python uses: the
  serialized row set is empty.
- Every other row is unchanged in content. The `anchor=` label moved from the
  fire's anchor block to the row's own block for matcher products (a row has no
  fire to name), so composite lines differ in that prefix only.

Verified at re-baseline:

- `--actions` vs `twins/py_actions.py` diffed **0 lines** over all 165 fixtures,
- `--actions-schema` and `--actions-prod` were **byte-identical** to `--actions`
  (the cross-check that catches a `fill_trace_aggregates` regression).

Row ORDER is sorted on both sides deliberately: Python's `compact_connections`
rebuilds `next_blocks` through a `set()`, so its spine order varies run to run,
and nothing downstream of the serializer depends on row order (they are DB rows
keyed by `action_id`).

## Using it

```
ton-mch-engine --actions -T indexer/tests/traces \
  | diff - ton-index-worker/ton-mch-engine/goldens/actions.golden
```

A diff is a **regression**, not a re-baseline. Update this file only through
review, with the reason for every changed row stated in the commit.
