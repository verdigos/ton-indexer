# T12.2 — first real protocol through the ABI path: DeDust CPMM V2 `claim_reward`

Authoring wave: W12 (T12.2). Branch `ton-abi`. This is the FIRST production ABI
row: `.tolk` decl → ABI JSON → `ton-abi-gen` pair → bridge registry rows → mch spec
→ classified `dedust_v2_claim_reward` action, gated diff-0 against the Python twin.

Read first: docs/ton-abi-plan.md §0.11/§0.16 + W12 wave log; ton-mch-engine/W12-BRIDGE-DESIGN.md
(acked T12.1 — trampoline/adapter/registry as-built); ton-abi/W11-SHAPE-SPEC.md;
indexer/dedust_abis/cpmm_v2_abi.talk (AUTHORITATIVE tolk source); cpmm_v2_matcher_spec/05,07;
docs/mch-cpp-plan.md §1 (gate set); indexer/indexer/events/blocks/dedust_v2/claims.py +
common.py (REFERENCE semantics only — see F2).

**PHASE-1 DELIVERABLE.** No code until ack. Findings + decisions below; STOP items in §7.

---

## 0. Protocol facts (verified against the AUTHORITATIVE .talk + survey)

`claim_reward` chain (cpmm_v2_abi.talk lines 207-227, survey `dev_utils/claim_reward_survey.md`,
60 traces / 2 clusters):

```
User ─▶ Pool        ClaimReward         0x909fdb65  queryId u64, rewardIndex u2, excessesTo addr
  Pool ─▶ Position  ClaimPositionReward 0x2e0cccba  (matched by opcode ONLY; body NOT parsed)
    Position ─▶ Pool PayoutReward       0x9e17fbbe  queryId u64, amount coins, rewardIndex u2,
                                                     ownerAddress addr, excessesTo addr   ← exact amount+owner
      payout leg  (jetton_transfer  |  PayoutMessage 0x3216ca09)   ← optional (zero-amount ⇒ absent)
      excess      (0xd53276db, matched by btype)
```

Two observed shapes (both are the SAME structural chain, differ only in payout leg):
- **Jetton payout** (cluster 000, n=59): leg = a `jetton_transfer` block. Fixture present:
  `tests/traces/i-gQwY3nWC6BQx1XOSWliBC-b1Pt7Y-Fn5jphK5uCGk=.lz4`.
- **TON payout** (cluster 001, n=1): leg = `PayoutMessage 0x3216ca09`. Fixture
  `YoVdDeJnFOQU1hjowGB8td2IEKG12Y7NTSzgqPkrIvU=` — **MISSING locally** (§7 STOP-1).

Action semantics MIRROR the old-engine `DedustV2ClaimRewardBlockMatcher` (claims.py:112):
- btype/action.type = `dedust_v2_claim_reward`; composite reuses `dex_withdraw_liquidity_data`.
- `amount1` = `PayoutReward.amount`; asset1/wallets from the payout leg (TON ⇒ `ton_asset()`,
  wallets null; jetton ⇒ `jt.data.asset/sender/sender_wallet/receiver_wallet`).
- side-2 fields all null; `lp_tokens_burnt` null; `is_refund` false; `reward_index` carried in `extra`.

---

## 1. The Python-twin problem (Q1 — CENTRAL). DECISION: option (a).

The mch A/B gate is **Python mch runtime vs C++ engine, diff-0 over the same `.mch` spec**.
C++ parses the body via ABI-generated structs + the acked adapter (`abi_value_to_mch`,
W12-BRIDGE-DESIGN §3). The Python mch runtime has NO ABI parser — it resolves
`parse CAP as Name` through `Registries.message_types[Name]`, a class constructed from a
pytoniq `Slice`, whose attributes are read by plain `getattr` (`expr_eval._access` line 353).

| Option | Verdict | Why |
|---|---|---|
| **(a) hand-written Python parser classes registered under the mch name, emitting the EXACT adapter value-shape** | **CHOSEN** | This IS the established mch pattern (every existing `parse` name maps to a hand pytoniq class in `blocks/messages/`). 3 flat structs, ~40 LOC. The only new discipline: **tolk-faithful field names/value kinds** so `body.<field>` resolves identically on both sides. |
| (b) drive the pinned TS reference pkg via node as the Python parse oracle | REJECTED | subprocess-per-message in the hot classify/twin loop; node in the pytest path; serializes cells across a process boundary; enormous latency + a new runtime dep for zero fidelity gain over (a). |
| (c) C++-only gating (no twin) | REJECTED | abandons the diff-0 bar that every other mch layer holds; the whole engine's correctness contract is the Python twin. Not acceptable for a first row that must set the pattern. |

**Shape contract that makes (a) work** (the load-bearing detail). The adapter maps a parsed
ABI struct to `mch::Value::Obj{ "$"=DeclName, <fields in decl order> }` with **tolk-faithful
camelCase field keys** and these per-kind value types:

| ABI field kind | C++ adapter yields | Python parser attr must yield | spec reads via |
|---|---|---|---|
| `coins` / `uintN,intN` (amount, queryId, rewardIndex, exitCode) | `Int` (RefInt256) | `int` (`load_coins`/`load_uint`) | `amount(...)` for coins; raw for indices |
| `address` (excessesTo, ownerAddress) | `Account` (`make_account_raw`, uppercase raw `wc:HEX`) | `AccountId` (`AccountId(slice.load_address())`) | **read directly, no `account()` wrapper** |
| `cell?` (payload maybe-ref) | `Null` or `Cell` | `None` or pytoniq `Cell` | not read by this spec |

Field NAMES are the binding constraint: spec accessor == adapter Obj key (ABI camelCase) ==
Python attr. So the Python classes use `queryId/rewardIndex/amount/ownerAddress/excessesTo`
(NOT the snake_case of legacy `blocks/messages/*.py`). Addresses are exposed **already-typed
as `AccountId`** to mirror the adapter's `Account`, and the spec reads them directly — exactly
like `jt.data.sender` (an `AccountId`) is read unwrapped in `dedust_withdraw.mch`. This sidesteps
any `account()`-idempotency question. **#1 phase-2 A/B verify:** `AccountId(pytoniq_Address).as_str()`
(Python render, py_actions.render line 197) must byte-equal the C++ adapter `Account` render from
`make_account_raw(wc:hex)`. Both are `wc:HEX_UPPER`; high confidence, but it is THE thing to prove
first. Fallback if it diverges: expose address as raw string + wrap `account(...)` in the spec
(then verify C++ `account(Account)` idempotency instead).

Baseline untouched: (a) adds new registry rows + a new spec + a new action type; it changes NO
existing parser, spec, or fixture. The 147-fixture corpus is only affected via the ONE
claim_reward fixture already resident (§0, §7).

---

## 2. Tolk compile flow (Q2). DECISION: minimal DECL file, compile via pinned tolk-js 1.4.1.

`cpmm_v2_abi.talk` carries a `tolk 1.1.0` pragma and 60+ decls (Q120X120 varuint, maps, unions,
recursive `SwapStep`). We need exactly **three** message structs, all flat:

```tolk
tolk 1.4.1
struct (0x909fdb65) ClaimRewardMessage  { queryId: uint64; rewardIndex: uint2; excessesTo: address; }
struct (0x9e17fbbe) PayoutRewardMessage { queryId: uint64; amount: coins; rewardIndex: uint2;
                                          ownerAddress: address; excessesTo: address; }
struct (0x3216ca09) PayoutMessage       { queryId: uint64; amount: coins; exitCode: int32;
                                          payload: cell?; }
```

- **DECL file over full-contract compile** because (1) guaranteed to compile — no maps/unions/
  recursion to stress the pinned compiler or the emitter; (2) the prod `mch-classify` lib gets a
  small generated pair (3 structs) not the entire 943-line contract; (3) matches plan §0.11
  ("author minimal .tolk DECLARATION files, structs + opcodes only"). Field names/opcodes are
  copied verbatim from the authoritative `.talk` so they stay faithful. Full-contract compile is
  a later expansion if more V2 actions land.
- **Pragma bumped to `tolk 1.4.1`** to match the pinned `@ton/tolk-js` — a 1.1.0-headered file
  fed to a 1.4.1 compiler is the untested axis; a decl file we author has no reason to pin the
  old version. (If 1.4.1 rejects the pragma form, drop the pragma line — decl files don't need it.)
- **Deterministic regen**: extend `ton-abi/tools/gen_vectors.mjs` fixture list (it already compiles
  `.tolk` → ABI JSON via `runTolkCompiler`, §gen_vectors:45) to emit `dedust_v2.abi.json`; assert
  `abi_schema_version == "1.0"`; commit both the `.tolk` and the `.abi.json`. `contract_name` =
  `dedust_v2` (⇒ namespace `ton_abi::gen::dedust_v2`, snake dir `dedust_v2_gen`).

---

## 3. Placement + registry identity (Q3, Q4-key). DECISION: prod pair, BARE-NAME keys.

- Generated pair `ton-abi/generated/dedust_v2_gen.{h,cpp}` — committed; `ton-abi-gen --check`
  extended to cover it (byte-repro guard, same as the 17 W11 pairs).
- **First real prod rows.** `AbiBridge.cpp` `abi_message_parsers()` (empty since T12.1) gains:
  ```cpp
  {"ClaimRewardMessage",  &abi_parse_body<ton_abi::gen::dedust_v2::ClaimRewardMessage>},
  {"PayoutRewardMessage", &abi_parse_body<ton_abi::gen::dedust_v2::PayoutRewardMessage>},
  {"PayoutMessage",       &abi_parse_body<ton_abi::gen::dedust_v2::PayoutMessage>},
  ```
  `AbiBridge.cpp` `#include`s `dedust_v2_gen.h`; `mch-classify` compiles `dedust_v2_gen.cpp` as a
  prod source (add to CMake; `ton-abi/generated` onto mch-classify include dirs). This flips the
  W12 state where generated pairs were test-only.
- **Registry key = the BARE decl name** (`ClaimRewardMessage`, …), NOT `<contract>::<Decl>`.
  **This is a conscious §0.16 deviation — flagged for ack.** Rationale: the `.mch` grammar's
  `parse CAP as Name` reads a bare IDENT; `::` lexes as two `COLON` tokens (lexer.py:25), so a
  qualified name cannot be written in a spec without a grammar change. All ~45 existing registry
  rows (hand + generated) are bare names, resolved by `parse_message_body(name, …)` (MsgParse.cpp:122).
  The `contract::` scheme's purpose was cross-contract same-name collision avoidance; none of these
  3 names collide with any existing key (checked: hand + `default_message_types` + generated).
  Duplicate protection still holds — `validate_registries()` / the `dups` set (T12.1) errors on any
  future collision. Revisit trigger: a genuine cross-ABI name clash ⇒ extend the grammar for a
  qualified parse-as name THEN. Cheaper and honest to stay bare for the first row.

---

## 4. mch spec (Q4). `specs/dedust_v2_claim_reward.mch`

```
opcode ClaimReward         = 0x909fdb65
opcode ClaimPositionReward = 0x2e0cccba
opcode PayoutReward        = 0x9e17fbbe
opcode DedustV2Payout      = 0x3216ca09

matcher dedust_v2_claim_reward {
    produces dedust_v2_claim_reward          # snake ⇒ btype identity ⇒ action.type
    entry    @claim

    op ClaimReward @claim
      -> op ClaimPositionReward @cpr
        -> op PayoutReward @payout_reward
          -> ( op DedustV2Payout @ton | btype jetton_transfer @jt )   # optional payout leg
          -> op ... excess handled by include-excess / btype excess

    parse claim         as ClaimRewardMessage
    parse payout_reward as PayoutRewardMessage
    parse ton           as PayoutMessage        # only when the TON arm bound
    out {
        dex:            "dedust_v2"
        sender:         account(claim.msg.source)
        pool:           account(claim.msg.destination)
        position:       account(payout_reward.msg.source)
        owner:          payout_reward.body.ownerAddress      # AccountId, read directly
        reward_index:   claim.body.rewardIndex               # raw int
        amount1_out:    amount(payout_reward.body.amount)
        asset1_out:     jt.data.asset if jt != null else (ton_asset() if ton != null else null)
        dex_wallet_1:   jt.data.sender if jt != null else (account(ton.msg.source) if ton != null else null)
        dex_jetton_wallet_1: jt.data.sender_wallet if jt != null else null
        wallet1:        jt.data.receiver_wallet if jt != null else null
        # side-2 + lp_tokens_burnt + is_refund handled in the _fill (constants)
    }
}
```

Exact leg/excess DSL (children-set vs single-chain, `include_excess`) will be finalized against
the fixture's real tree in phase 2 by mirroring `dedust_withdraw.mch` idiom (children set with
first-match-wins); the shape above is the contract. `ClaimPositionReward` is matched by opcode
only (never parsed) — mirrors the reference. `out{}` keys are chosen by me and consumed by the
new `_fill` (§5), so they are consistent by construction.

**Python message_types + lookups wiring:** new factory `builders/dedust_v2.py`
`dedust_v2_claim_reward_registries()` registering the 3 classes (opcodes + message_types) and
reusing the default `jetton_wallet` lookup; added to `_FACTORIES` in the three twins
(py_actions/py_classify/py_match_dump) and to the pytest spec loader. IR artifact: add the spec to
`ir/emit_manifest.json`, regen `ir/mch_ir.json` (`python -m indexer.events.mch.ir_emit`).

---

## 5. Action serialization (the real T12.2 tail — larger than the ABI parse itself)

The mch action-row path REUSES the real `block_tree_serializer.block_to_action` (py_actions.py:219),
and the gate emits only blocks whose btype ∈ `PORTED_BTYPES` (the 18 composites C++ `fill_action`
ports, Classify.cpp:556). `dex_withdraw_liquidity` is NOT ported; `dedust_v2_claim_reward` is new.
So a **gated** claim_reward action requires porting the fill on BOTH sides:

- Python: `_fill_dedust_v2_claim_reward(block, action)` in `block_tree_serializer.py` + a
  `case 'dedust_v2_claim_reward'` in `block_to_action`; fills `dex_withdraw_liquidity_data` from the
  out{} keys (amount1, asset1_out, wallets, dex_wallet_1, side-2 = null, lp_tokens_burnt = null,
  is_refund = false) and sets `action.source/destination/asset`; `reward_index` → `action.extra`.
- C++: `fill_dedust_v2_claim_reward(d, a)` in Classify.cpp + an `else if (t == "dedust_v2_claim_reward")`
  branch in `fill_action` (a.type = b->btype, line 36, already yields `dedust_v2_claim_reward`).
- Gate wiring: `PORTED_BTYPES += "dedust_v2_claim_reward"` (py_actions/py_classify), regen
  `cpp_surface.json` via `EXE --surface`, regen the twin expected dumps (classify/actions/match/
  parse-dump). `dex_withdraw_liquidity_data` composite columns already exist in `ACTION_COLUMNS`? —
  **NO**: the render column set (py_actions:115) lacks `dex_withdraw_liquidity_data`. Adding it to
  the rendered column set + the C++ `render_action` column set is part of the port (phase-2 detail;
  low-risk, follows the 18 examples).

This is well-trodden plumbing (18 precedents), but it IS the bulk of the LOC. Honest scope call:
T12.2 is MED on the NOVEL axis (ABI parse path) but touches ~8 files across three languages.

---

## 6. Owed T12.1 items — disposition

- **validate_registries() called from the mch-classify embedding entry** (W12 §4/§10, "embedding-actor
  validate seam noted"): **INCLUDE in phase 2.** With the first non-empty `abi_message_parsers()`,
  the dup-check becomes live and should run at the embedding startup, not just CLI `main`. Small,
  and the right wave to prove it (rows now exist to collide).
- W12 §9 open items: (1) exact abort-safe exotic-open helper in the trampoline — **RESOLVE in phase 2**
  when `abi_parse_body` is first exercised on a real body; (2) fixture-pair selection — **N/A**, the
  prod pair is dedust_v2 now; (3) `build_message_registry` unit exposure — already landed T12.1, reuse.

---

## 7. STOP-and-report findings (surface before phase 2)

1. **TON-payout fixture MISSING.** Only the jetton-payout fixture `i-gQwY3n…lz4` is present
   (untracked — it is the working-tree swap for the git-deleted `02kEPzkQ…lz4`, so the on-disk
   corpus is still 147). The TON-payout rep `YoVdDeJn…` and the other jetton reps are absent.
   Decision needed: (a) fetch `YoVdDeJn…` from the live hot/cold+kvrocks cluster (memory
   `hotcold_kvrocks_trace_fetch`, `save_trace --db cold --kvrocks`) so BOTH legs are fixture-gated;
   or (b) gate the jetton leg via the fixture and the TON `PayoutMessage` leg via a hand-built
   `--abi-bridge-test` unit (cell built in C++) — no TON fixture in the corpus. **Recommend (a)** for
   a real end-to-end TON row; (b) is the fallback if the fetch is impractical. I will NOT invent a
   TON fixture.
2. **Python `blocks/dedust_v2/` package is reference-only on this branch.** It is untracked and its
   imports (`DedustV2ClaimReward`, …) are unresolvable (`messages/dedust_v2.py` absent, only a stale
   `.pyc`). The old matcher cannot run here — it is a semantics reference, not a runnable twin. No
   blocker: we author fresh Python mch parser classes (§1). Confirms we do NOT reuse those classes.
3. **§0.16 registry-key deviation** (bare name vs `contract::Decl`) — §3. Needs explicit ack.

---

## 8. Gate set (phase-2 acceptance — all must stay/land green; STOP over tweak-toward-green)

- (a) mch gate set (mch-cpp-plan §1): classify/actions/match/parse-dump/schema/prod diff-0. These
  now MOVE (new spec + new action rows + the claim_reward fixture reclassifies): the expected dumps
  and counts (196 → 196 + N claim rows) regen as the normal protocol-addition procedure. Build via
  `twins/build_mch.bat` (PowerShell), twins from `indexer/` with venv python.
- (b) ton-abi ctest: `ton_abi_smoke` + `ton_abi_gen_check` (--check now covers `dedust_v2_gen`).
- (c) mch ctest incl. `mch_abi_bridge` (add claim_reward parse asserts) — green.
- (d) Python `pytest tests/mch tests/mch_ir tests/test_actions.py` — green (new spec case added).
- (e) `cpp_surface.json` updated (expected — first real surface change since T12.1).

---

## 9. Deltas vs the task brief (for the ack)

- Q1 → option (a), with the address-parity verification named as the #1 risk + the raw-string
  fallback.
- Q2 → minimal DECL file (not full-contract compile), pragma bumped to 1.4.1.
- Q3 → prod pair into mch-classify; **bare-name registry keys (§0.16 deviation, needs ack)**.
- Q4 → spec sketch + out{}→_fill contract; addresses read directly (unwrapped AccountId).
- Q5 → gating requires a NEW ported composite (`dedust_v2_claim_reward` + `dex_withdraw_liquidity_data`
  render columns) on both sides — the real bulk; jetton leg fixture-gated, TON leg per §7-STOP-1;
  validate_registries embedding seam INCLUDED (not re-deferred).
- Scope honesty: NOVEL work = ABI decl+gen+bridge rows (small, the point of T12.2). PLUMBING =
  serializer port + surface/twin regen across ~8 files (the bulk, 18 precedents). No commits.

---

## 10. AS-BUILT (phase 2 complete) — deltas from the acked design

All gates green (see §8). Deltas worth recording:

1. **#1 address-parity verify: PASS.** Both at unit level (`mch_abi_bridge`
   `dedust_v2/payout_reward/owner_addr_parity` asserts the parsed AbiAddress renders the
   exact `0:AAAA…` uppercase-raw string = Python `AccountId.as_str().upper()`) AND at full
   pipeline (classify + actions diff-0 over the real jetton fixture). The raw-string+`account()`
   fallback was NOT needed; the direct-AccountId design held.
2. **New Action field `dex_withdraw_liquidity_data`** added to the C++ `Action` struct + BOTH
   render column sets (Classify.cpp `render_action`, twins `ACTION_COLUMNS`) — the composite was
   present in the Python serializer but had never been in the mch render surface (only
   `dex_deposit_liquidity_data` was). Anticipated in §5.
3. **TON-payout fixture: NOT fetched → fallback (b) taken** (per lead timebox). One honest
   attempt: this tree's `tools/save_trace.py` has NO `--kvrocks`/`--db` flags (they live only in
   the n2 copy), the kvrocks host does not resolve (no VPN), and there is no monorepo-root `.env`.
   The TON `PayoutMessage 0x3216ca09` leg is instead **unit-gated** in `mch_abi_bridge`
   (`dedust_v2/payout_msg/*`: parse + adapter + null-payload). The jetton leg is fully
   fixture-gated (`i-gQwY3n…`, classify+actions diff-0). A YAML/fixture TON case can follow when
   the fixture lands.
4. **`.gitignore` gotcha (COMMIT-BLOCKER):** `indexer/events/mch/builders/dedust_v2.py` is caught
   by `.gitignore:314` `[Bb][Uu][Ii][Ll][Dd]*` (matches the `builders` path component). The
   existing builder files are grandfathered (already tracked). The new one needs
   `git add -f indexer/events/mch/builders/dedust_v2.py` at commit, else the twin/pytest break on
   a clean checkout.
5. Action count 196 → **197** (the resident jetton fixture reclassifies unknown→dedust_v2_claim_reward,
   a clean +1). `--surface`/`cpp_surface.json` updated (3 parser rows; normal protocol-addition).
6. Registry keys BARE (acked §3): rows `ClaimRewardMessage` / `PayoutRewardMessage` / `PayoutMessage`.
7. **No pytest case was added** — §8(d) ("new spec case added") did NOT happen and the
   W12 wave-log line should not be read as claiming one. The protocol is gated by the
   TWINS (classify + actions diff-0 over the real jetton fixture, which is the stronger
   bar: it runs the whole Python engine against the whole C++ engine) plus the
   `mch_abi_bridge` units. A `tests/mch` spec case would only re-assert what the twin
   gate already proves end-to-end. R1b added `tests/mch/test_abi_twin_fields.py`, which
   pins the twin parsers' field names/order against the committed ABI — the one thing
   the twins cannot catch cheaply (a rename shows up only as a full-corpus diff).
8. **`out{}` shrank from the §4 sketch**: `position:` and `owner:` were dropped. Neither
   has a home in `dex_withdraw_liquidity_data`, and the `_fill` never consumed them —
   `payout_reward.body.ownerAddress` is therefore parsed but unused. Recorded here
   because §4 still shows them.
9. **tolk pragma: `tolk 1.0`, not the 1.4.1 §2 specified.** The as-built
   `abi/dedust_v2.tolk` carries `tolk 1.0` and compiles + `--check`s byte-identically
   under the pinned `@ton/tolk-js`. Left alone deliberately: bumping the pragma would
   force a regen of the committed `.abi.json` AND the generated pair for no behavioral
   gain. `abi/_TEMPLATE.tolk` (R1b) matches the as-built pragma and says so.
