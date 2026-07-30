# Frozen action goldens

`actions.golden` is the committed expectation for

```
ton-mch-engine --actions -T indexer/tests/traces
```

run from the repo root (no `-I`: the matcher table is compiled in since EM4).
**176 fixtures, 731 ACTION rows** — one per serializable block on each trace's
post-pass spine, plus 6 synthetic `unknown` rows (the fixtures whose spine
serializes to nothing at all). Rows are printed **sorted** within a fixture,
because spine order is not a stable property (see below).

## Why it exists

It replaces the live Python twin (`twins/py_actions.py`) as the reference for
the fixture actions gate. The twin imports the real Python classifier, so it
dies with it — and Python's `--emulated-traces` path is being retired. This file
is the last snapshot taken while both engines could still be compared.

## Provenance

Re-baselined 2026-07-30 (**745 → 731 rows**, and EVERY row gains one column) by
the `nft_mint` port (`specs/nft_mint.mch`, the mch counterpart of
`NftMintBlockMatcher`). Two independent effects:

- **Column**: `nft_mint_data` joins the rendered column set (`Classify.cpp`
  `render_action` + the twin's `ACTION_COLUMNS`), so all 731 lines carry a new
  `nft_mint_data=null` between `nft_listing_data` and `opcode` — except the three
  `nft_mint` rows, which carry `{nft_item_index=…}`. This is the only whole-file
  churn in the diff.
- **Rows**, over exactly 6 fixtures (`-13 call_contract`, `-4 jetton_transfer`,
  `+3 nft_mint`, 3 `tonco_deposit_liquidity` rows rewritten):
  - three fixtures (`00wjHu7i…`, `GO-NI0Ej…`, `K_PlZB5R…`) swap the deploying
    `call_contract` leaf for the `nft_mint` row it now builds;
  - three tonco fixtures (`0--IKFmIb…`, `0GW2kNF4…`, `0_JGy9zy…`) lose the legs
    that `tonco_deposit_liquidity` can finally absorb — that matcher binds
    `btype nft_mint`, so until this port it was shipped-but-dead and emitted a
    row with `nft_address` / `nft_index` / `lp_tokens_minted` all null. Those
    three rows are now filled (and their tx_hashes/lt ranges widened).

Re-baselined 2026-07-30 (**765 → 746 rows**) by the `dedust_v2` swap port
(`specs_decl/dedust_v2_swap.mch`, the mch counterpart of
`DedustV2SwapBlockMatcher` + `DedustV2FailedSwapBlockMatcher`). DeDust CPMM V2
swaps were entirely unclassified: every one of them serialized as its raw legs.
10 fixtures changed:

- **-14 `call_contract`, -15 `jetton_transfer`, +10 `jetton_swap`** — seven
  successful swaps (three of them multi-hop, with `peer_swaps` filled from the
  per-hop Swap events) and three slippage refunds, each absorbing the leg rows
  that used to stand in for it.
- **9 of the 10 became byte-identical to the LEGACY Python classifier**
  (`event_processing.matchers` driven through the twin's renderer), including the
  `(ab-gap)` fixture below. The tenth (`MZQz_qHLV8Oi…`) matches legacy on its
  dedust_v2 row and still differs on an unrelated **dedust V1** swap row in the
  same trace, whose leg wallets the C++ host leaves null — the same pre-existing
  gap the pton re-baseline recorded, byte-identical to the previous golden.

Re-baselined 2026-07-30 (**818 → 765 rows**) by the `pton_transfer` matcher port
(`specs_decl/pton_transfer.mch`, the mch counterpart of `PTonTransferMatcher`).
pTON wallets speak the jetton transfer interface but emit no internal transfer,
so until this port every pTON leg stayed an unconsumed `call_contract` and the
DEX matchers that bind `btype jetton_transfer` lost their TON side. 15 fixtures
changed:

- **-58 `call_contract`, +10 `jetton_swap`, -5 `jetton_transfer`** — the swaps
  those legs unblocked (stonfi v1 and v2), each absorbing the call rows that used
  to stand in for them. The 5 lost `jetton_transfer` rows are pton products a
  swap now consumes; the surviving ones are pton legs no matcher claims, which
  Python emits as their own rows too.
- **11 of the 15 became byte-identical to the LEGACY Python classifier**
  (`event_processing.matchers` driven through the twin's renderer), including both
  `(ab-gap)` fixtures below. The other four already diverged from legacy before
  this change, for reasons it does not touch (matchers absent from the spec set;
  a dedust swap whose leg wallets the C++ host leaves null) — their diverging rows
  are byte-identical to the previous golden.

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

Verified at each re-baseline (the 165-fixture figures are from the spine-walk one):

- `--actions` vs `twins/py_actions.py` diffed **0 lines** over all 165 fixtures
  (**176** at the pton and dedust_v2 re-baselines),
- `--actions-schema` and `--actions-prod` were **byte-identical** to `--actions`
  (the cross-check that catches a `fill_trace_aggregates` regression).

Row ORDER is sorted on both sides deliberately: Python's `compact_connections`
rebuilds `next_blocks` through a `set()`, so its spine order varies run to run,
and nothing downstream of the serializer depends on row order (they are DB rows
keyed by `action_id`).

## The 11 A/B-gap fixtures (added 2026-07-30, +31 rows)

Recorded from finalized mainchain blocks 82925391-82931345 with
`ton-fixture-save`, one per divergence class found in the live emulator A/B run
(C++ in-process classifier vs Python `event_classifier`). Python's rows are the
parity target and are asserted by pytest cases named `(ab-gap)` in
`indexer/tests/test_cases/`. **Three are FIXED**, three still carry a genuine C++
gap — those three golden rows are KNOWN-WRONG today and a matcher port should flip
them:

- ~~`mg8sRwic_zM8iy-pZoVdocmQsozeVKNt7G5j_k6nYyc=` pton stonfi v2 swap~~ — **FIXED**
  by `specs_decl/pton_transfer.mch`: 6x`call_contract` -> 1 `jetton_swap`, byte-identical to Python.
- ~~`2xripwtdxwKPdHABREASj6enBvKBTwsOVJx6YEZfI64=` pton stonfi v1 swap~~ — **FIXED**
  by the same port: 5x`call_contract`+2x`jetton_transfer` -> `call_contract`+`jetton_swap`+`jetton_transfer`, byte-identical to Python.
- ~~`2UJH7bBZDI4vmFG7O7HgraaQQF0BfjS2Eg5JaY62_Zo=` dedust v2 swap~~ — **FIXED**
  by `specs_decl/dedust_v2_swap.mch`: 2x`call_contract`+`jetton_transfer` -> `call_contract`+`jetton_swap`, byte-identical to Python.
- ~~`4AAiT_8jTvxhujXOywe0zZv1sylSlAwnZV9stEG3gnM=` teleitem start auction~~ — FIXED by specs/teleitem_auction.mch: call_contract -> teleitem_start_auction, byte-identical to Python
- ~~`rV1gzQRFXipz6XEByCTq_3Q2LZpa4i61aLsvHf1CR1A=` teleitem cancel auction~~ — FIXED by specs/teleitem_auction.mch: 2x call_contract -> teleitem_cancel_auction, byte-identical to Python
- `5b-KXr82wUSLIHPSHkFaW791xTyN0Rmu5h0mFm_4IdQ=` nft transfer — 3x`call_contract`, want 1 `nft_transfer`

The other five **already match Python here**, so their golden rows are correct and
they are regression guards, not pending work. Note what that means: the class
they were recorded for does NOT reproduce in this harness.

- `o6-kGFnpDMI4nMlMH1zrNNijL0taVyegbi2KlWkgJ-8=` stonfi v2, `PdxEcev5rXzXI-fHb1VPfFA1eLcm3n4Mx6NbsuATc3A=` tonco,
  `YBQYgsltVHCigC9q03KhXnBgho2bsxFuvOD8u6UC_J4=` dedust v1 failed —
  recorded as **host rejects**. All three emit the right `jetton_swap` off a
  fixture. The live rejection came from the host lookup tier (celldb/redis
  interface fetch), which `FixtureLookupSource` serves from the fixture instead,
  so the fixture cannot reproduce it. Chasing those needs a host-layer test, not
  this golden. A fourth host reject (stonfi v1, `Lo6cMzpA…`) was recorded and then
  deleted: only the host knows the intended out-asset, which sits on a pool-side
  wallet the trace never touches, so the fixture baked in a wrong `asset2` and the
  case asserted it. The class stays fixture-untestable until the format carries
  raw account state (fixture-v2).
- `TXFkYzpo-8IZvMcy8lhsrKG9QJH73JaYAwsAitgNqdM=` (4x`call_contract`) and
  `sS0j8bD7JQj7389dsGXLdNUisE6I1ApyvdPM1gkODVc=` (`call_contract`+`ton_transfer`) —
  recorded because the live A side emitted FEWER basic rows than B. That is the
  shortfall 362ff33 fixed by moving the row set to the `serialize_blocks` spine
  walk; the A/B snapshot predates it. Kept as guards for that fix.

## Using it

```
ton-mch-engine --actions -T indexer/tests/traces \
  | diff - ton-index-worker/ton-mch-engine/goldens/actions.golden
```

A diff is a **regression**, not a re-baseline. Update this file only through
review, with the reason for every changed row stated in the commit.
