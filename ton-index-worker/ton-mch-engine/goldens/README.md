# Frozen action goldens

`actions.golden` is the committed expectation for

```
ton-mch-engine --actions -I ir/mch_ir.json -T indexer/tests/traces
```

run from the repo root. **165 fixtures, 277 ACTION rows** (227 matcher-produced
+ 50 synthetic `unknown` rows, one per fixture that classifies cleanly and
matches nothing).

## Why it exists

It replaces the live Python twin (`twins/py_actions.py`) as the reference for
the fixture actions gate. The twin imports the real Python classifier, so it
dies with it — and Python's `--emulated-traces` path is being retired. This file
is the last snapshot taken while both engines could still be compared.

## Provenance

Frozen 2026-07-29 from a run where

- `--actions` vs `twins/py_actions.py` diffed **0 lines** over all 165 fixtures,
- `--actions-schema` and `--actions-prod` were **byte-identical** to `--actions`
  (the cross-check that catches a `fill_trace_aggregates` regression).

## Using it

```
ton-mch-engine --actions -I ir/mch_ir.json -T indexer/tests/traces \
  | diff - ton-index-worker/ton-mch-engine/goldens/actions.golden
```

A diff is a **regression**, not a re-baseline. Update this file only through
review, with the reason for every changed row stated in the commit.
