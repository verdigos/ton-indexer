# MCH code-minimization program — state document

**Branch:** `mch-minify` (from `acd84409`, forked off `mch-engine-v2` 2026-07-12).
**Purpose of this doc:** SESSION-CORRUPTION INSURANCE. Every finding, decision, and wave
status lives here. Updated after EVERY wave. A fresh session continues from this doc alone:
read it top to bottom, then plan §0/§1 of `mch-cpp-plan.md` (ground rules + gate set), then
pick up at the first wave whose status isn't DONE.

## 0. User decisions (2026-07-12, explicit)

1. **Program scope**: start SAFE WAVES now (M2/M1/M5 — C++-side, byte-identical), language
   wave approved too (see 2-3), report order.
2. **Language revival: G-LC `map(xs as e => expr)` + G-Q `any/all(xs as e => bool)` = YES,
   both.** (These were user-deferred; revival now explicitly authorized. Spec §16.1's own
   trigger condition satisfied by measurement.)
3. **parse-as-expression (`let m = parse x as T`) = YES in principle, scheduled LAST**,
   batched with IR v1.1, only after G-LC/G-Q prove out.
4. **Timing**: minimization runs PARALLEL to the shadow-run track (shadow = mch-engine-v2
   branch, remote server, user-driven; flip gate independent of this program).
5. **W7 pre-authorization (user, 2026-07-12 — W7 needs NO further check-in):**
   (a) parse-expression failure semantics = FAULT → build reject, exactly like statement
   form; single form, NO nullable `parse?` variant. (b) IR v1.1 = MINIMAL: only the new
   parse expr node kind + strict version bump (no dual-accept; artifact + both loaders +
   fixtures regen one commit); no other IR items ride along. (c) DEX-core shrink pass:
   attempt ALL protocols (stonfi_v2/tonco/dedust), keep only diff-0 outcomes, park
   resisters with a report — no per-protocol check-ins. (d) Abort rule: if the resolver
   needs new capture-scoping semantics (cost →M+), PARK W7 automatically, ship what's
   green, report the scoping design as a proposal.
6. Standing (from earlier sessions, still in force): commits per wave after lead
   verification; one-line messages, no Co-Authored-By; pre-existing working-tree changes
   (core/database.py, core/settings.py, serializer hunks, tgbtc.yaml, deleted lz4,
   ton-index-go, submodule ptrs) stay OUT of commits; -j4 build cap; PowerShell +
   twins/build_mch.bat only; no Bash heredocs for C++; no aborts; Python = ground truth;
   never break a green gate (plan §1 = 12 gates, current numbers: vectors 97×2, build-vec
   35, parse 0, match 328, classify 196, actions 196, schema 0, prod 0, ctest 5/5, pytest
   1950/2, 0 aborts).

## 1. Source material

- `docs/mch-cpp-host-runtime-findings-2026-07-12.md` — third review (host/runtime seam;
  candidates 1-6; typed hosts / DEX records / block views / lookup-wave / facade / typed-IR
  codegen). Lead triage of it: C1+C2 adopt (pilot Coffee), C3 fold-in, C4 = decision
  recorded below (§2 item L1), C5 defer to persistence slice, C6 parked (3rd rejection).
- **fable-minify report 2026-07-12 (full substance in §3 below — the program's spec).**
- Prior reviews: `docs/mch-review-untouched-2026-07-12.md` (A/B/C items — ALL adopted items
  already landed on mch-engine-v2 through acd84409), `docs/mch-cpp-fresh-architecture-review-2026-07-11.md`.

## 2. Standing design decisions (lead, recorded)

- **L1 (lookup/tier-2, from C4 discussion)**: when tier-2 (Postgres/kvrocks) gets wired,
  preferred shape = **rerun-with-prefilled-table**, NOT a resumable/coroutine API:
  classification is pure + ~3ms/trace; run tier-1-only → collect tier-2 misses → batch
  fetch → re-run trace with FilledLookupTable pre-seeded. Tier-2-only kinds are rare
  (nominator_pool, jvault_assets; failed-swap jetton_wallet keys are the volume case —
  body-derived addresses not present in trace → tier-1 miss guaranteed). Typed lookup
  identity + result taxonomy (found/miss/malformed/infra) adopted when that wave happens.
  NOT part of this program; recorded so nobody wires a sync DB call into the actor.
- **L2**: `in` operator + division stay CUT (no demand found in any host fn).
- **L3**: fold/zip-with-next NOT added (coffee adjacent-hop + stonfi prev_out threading
  stay in host cores — language cost > ~80 LOC deleted).
- **L4**: parse-gated predicates NOT added (evaa ×4, pton_self_transfer,
  stonfi_v1_sender_payment stay C++ preds — new phase semantics not worth ~180 LOC).

## 3. The fable-minify report (verbatim substance — program spec)

### 3.1 Measured inventory

- src/parse/* + MsgParse.cpp + tlb = 1,993 LOC; src/host/* = 4,511 LOC.
- **~810 LOC of host/* is hidden MESSAGE PARSING** (hand cursor-walk decoders inside
  HostStonfi 340, HostTonco 155, HostDedustDeposit 130, HostNft 100, HostCoffee 95,
  HostTgbtc+HostEvaa 70). Real split ≈ 2,800 parsing vs 3,700 host logic.
- Parse layer detail: infra (PSlice.{cpp,h}, Parsers.h, MsgParse.cpp) 503 KEEP; tlb+tlbc 59
  (jetton family only — tlbc adds little, M1 emits checked-cursor code directly);
  ParseJettons 283 (5 regular + forward-payload sum-type tail irregular + parse_never);
  ParseDns 186 (hand forever); ParseLayerZero 152 (hand forever); ParseDedust 136 (2
  regular + steps chain walk); ParseJvault 109; ParseEvaa 90 (3 regular; fail_excess =
  opcode→enum map); ParseVesting 79 (int_msg_info prelude candidate); ParsePton/Coffee/
  Stonfi 146. 21 registered parsers, 26 parse stmts in artifact; **16/21 fully generable**.
- **Fetch vocabulary CLOSED** (each = one existing PSlice helper): u32/u64/uN/iN,
  coins(load_coins_py), addr_py(load_address_py), maybe_ref→cell|bytes, remainder→cell,
  asset4 (dedust), asset2 (coffee), dict_keys(267)→[account], snake→bytes,
  enum(u32){...} else-fail, tail hook NAME (hand continuation).
- Host fn taxonomy: 16 protocol fns EACH CALLED EXACTLY ONCE in artifact; 11 preds; 4
  shapers. Per-fn (i)record/(ii)topology/(iii)irreducible split and disposition:
  stonfi_v2_swap_data 335 stays −50%; tonco_swap_data ~290 stays −45%;
  tonco_deposit_liquidity_data ~250 stays −45%; dedust_deposit final/partial ~500 stays
  −45%; dedust_swap_legs ~210 stays −40% (seq_destination lookup fold = THE irreducible
  async fn per spec §16.1); stonfi_v1_swap_data 185 stays −50%; nft_transfer_data ~155
  stays −35%; telegram_nft_purchase_data ~120 stays −40%; coffee_swap_data ~230 stays −45%
  (const_cast peer fill = zip-with-next pattern → typed records kill the const_cast);
  **getgems_nft_purchase_data ~50 → MOVES TO SPEC** (95% record; payout selection stays as
  existing pred); **tgbtc_mint_data ~85 → spec** (txid_le_hex stays tiny fn);
  **jvault_account_list ~35 → G-LC** (its own comment says it exists only because G-LC
  deferred); **jvault_stake_period ~45 → generated payload parser**;
  **dedust_swap_failed ~28 → G-Q** (`any(swaps as s => not any(children(s, notif)))`);
  **tonstakers_minted_nft ~25 → pattern optional-child capture (TODAY's language)**;
  layerzero_dst_oapp_matches ~30 irreducible (spec §15 keeps it).
  Preds: no_internal_transfer possibly spec-expressible today (verify resolver);
  parse-inside-predicate fns stay (L4). Shapers = graph surgery, hand forever.

### 3.2 M1 — message-schema compiler (concrete design)

INPUT: one `mch-msgs.schema` in ton-mch-engine/ — declarative logical-output DSL over the
closed fetch vocabulary. Example:

```
message StonfiV2PayTo opcode 0x657b54f5 policy skip-unchecked {
  _skip: u64
  _to: addr_py; _exc: addr_py; _orig: addr_py   # parsed-and-dropped; failure still fails whole parse
  exit_code: u32 -> int
  _cp: maybe_ref
  additional_info: ref {
    _fwd: coins
    amount0_out: coins -> int
    token0: addr_py -> account
    amount1_out: coins -> int
    token1: addr_py -> account
  }
}
message DedustSwapStep {            # M5 chain-walk construct
  pool: addr_py -> account_or_addr_none_str
  _flag: bits 1
  _limit: coins
  next: chain maybe_ref DedustSwapStep   # generator emits a LOOP, not recursion
}
```

EMITS (deterministic, content-stable, same CMake sha-pairing as builds codegen):
1. C++ parser fn per message calling the SAME PSlice helpers (zero new leaf semantics).
2. message_parsers() registry rows + Parsers.h decls (kills MsgParse hand map + 7-way
   metadata duplication).
3. Descriptor table (name/opcode/policy/fields) → feeds --surface, parse-dump, artifact
   capability check.
4. Per-descriptor test vectors (min/max/truncation-per-field/addr forms) — closes the
   parser-coverage gap for free.

CANNOT GENERATE (~350 LOC hand residual): jetton_transfer forward-payload tail (sum-type +
copy() ref-reset quirk → `tail hook`), ParseDns, ParseLayerZero, vesting int_msg_info
(prelude-type candidate later), PSlice infra. parse_never = descriptor flag
`constructible: false`. Generator ~800-1,200 LOC Python beside ir_emit, same
manifest/repro discipline as the A1 emit manifest.

COVERAGE: 16/21 registered + ~14 host-embedded decoders + 3 chain walks ≈ 1,600 of 1,990
parse LOC → ~250 schema lines. GATES: parse-dump twin diff 0 unchanged; new ctest over
generated vectors; artifact untouched. Python keeps pytoniq parsers as ground truth;
generating the Python twin from the same schema = later option after differential fuzzing.
SEMANTICS TRAP handled: host-embedded parses keep per-fn error policy (rt_fault vs
Null-reject vs pred-False) — parse_message_body returns same td::Result, caller keeps
policy, byte-identical.

### 3.3 M2 — shared DEX records + typed adapters + block views (quantified)

- Leg dict {asset, amount, source, source_jetton_wallet, destination,
  destination_jetton_wallet}: built 9× (HostStonfi 3, HostTonco 2, HostDedust 2,
  HostCoffee 2) ≈ 135 LOC → TransferLeg record + one encoder.
- peer_swap {in:{asset,amount}, out:{...}}: 5× ≈ 60 LOC.
- Top swap record {dex, sender, source_asset, destination_asset, dex_incoming_transfer,
  dex_outgoing_transfer, referral_amount, referral_address, peer_swaps, failed|success}:
  5× ≈ 60 LOC → SwapRecord::encode() owns names + null conventions; kills HostCoffee
  const_cast mutation (peer out-asset fill becomes typed pass pre-encoding).
- pton master list + wallet→asset conversion duplicated VERBATIM HostStonfi:46-56 vs
  HostTonco:36-51 (+3 variants) ≈ 80 LOC → one helper with pton_conversion flag (v1
  deliberately does NOT convert — parameter, documented).
- Arg-decode preamble ×16 ≈ 200 LOC → typed adapter
  `HostResult<SwapRecord> fn(HostContext&, ConsumedBlocks)`.
- View helpers (find_call/first_call_op/first_jt_child, lt-stable-sort, acc_str ×3,
  data-truthiness ×6) ≈ 150 LOC → ConsumedView::calls(op)/.by_type(t)/.lt_sorted(),
  BlockView::data_truthy(f).
Net ≈ −800 +450. Pure C++ internal; all gates byte-identical + new typed unit tests.
Pilot = Coffee.

### 3.4 M3 — language wave (G-LC + G-Q + parse-as-expression)

- G-LC `map(xs as e => expr)`: deletes jvault_account_list; enables dedust peer_swaps +
  record literals over captured lists.
- G-Q `any/all(xs as e => bool)`: deletes dedust_swap_failed; expresses stonfi/tonco
  per-hop exit-code checks.
- parse-as-expression `let m = parse x as T` (IR v1.1): the enabler — G-LC without
  per-element parse can't touch swap-hop loops. Also fixes hidden-mutable-state parse model.
- Cost: G-LC+G-Q ≈ frontend ~120 + py runtime ~60 + C++ interp ~60 + codegen ~60 + vectors;
  deletes 350-500 LOC PER RUNTIME (spec-side moves delete BOTH twins).
- Gate: Python-first (ground truth), new expr vectors, then C++ interp + codegen; spec
  edits moving fn bodies into out{} must yield classify/actions diff 0 (semantic no-ops).
- Soft spot: assumes resolver can bind captured lists in map without new capture
  semantics; if list-under-recursion scoping complicates, cost grows to M.

### 3.5 M4 — moves possible with TODAY's language (batch with M3 artifact wave)

- tonstakers_minted_nft → optional child capture (opcode 0x132F9A45) + .msg.destination.
- getgems_nft_purchase_data → 18 let/field copies in spec out{}; payout pred exists.
- tgbtc_mint_data → pattern captures + record out{}; keep fn bitcoin_txid(int)->str.
- no_internal_transfer → inline where on .data IF resolver permits (verify; else stays).

### 3.6 M5 — chain-walk schema construct

dedust steps + stonfi_v2 pool_accounts cross-swap + tonco collect_payload_targets = one
linear tail-list-with-continuation-ref construct (~130 deleted, ~120 in generator).
Depth-guard generator-emitted.

### 3.7 Stays HAND-WRITTEN forever

Six DEX cores (conditional dual-dispatch legs, failure fallbacks, dependent lookup folds —
expressing in MCH = reinventing a general-purpose language; they shrink 40-50% via M1/M2);
dedust_seq_destination (dependent multi-hop lookup fold); shapers (walker-invariant
mutation); quirk shims ≈500 LOC pinned pytoniq parity (py_int_base0, cell_from_pystr,
txid_le_hex, layerzero compare, fragment-URI split, PSlice, evaa header preds);
ParseDns/ParseLayerZero.

### 3.8 End-state numbers (honest)

parse+host written 6,254 → ~3,900-4,200 (−35-38%): host ~2,600-3,000 + parse hand ~860 +
schema ~250. Marginal cost per new DEX protocol: 300-1,000 LOC ×2 runtimes → ~30 schema
lines + 100-250 LOC core ×2. Soft spots: DEX-core shrink ±15% (stonfi_v2 failure-fallback
interleaving may resist); generator LOC is shape-guess until DSL grammar fixed.

## 4. WAVE PLAN + STATUS (update after every wave)

Gate rule: every wave ends with the FULL plan §1 gate set green (current numbers §0.5) +
wave-specific gates. Lead verifies fresh + commits per wave. Doc updated same commit.

| Wave | Content | Gate | Status |
|---|---|---|---|
| W1 | M2 pilot: DexRecords module (TransferLeg/PeerSwap/SwapRecord + one encoder) + typed host adapter (HostContext, ConsumedBlocks) + first view helpers + convert HostCoffee (incl. const_cast kill) + pton/wallet→asset shared helper | all byte-identical + typed unit tests (ctest) | **DONE** (commit: see log) |
| W2 | M1 generator: DSL grammar + Python generator (beside ir_emit, manifest/sha discipline) + emit the 16 regular REGISTERED parsers + descriptor table + generated per-parser vectors + ctest; delete replaced hand parsers | parse-dump diff 0; new vectors ctest; all 12 gates | **DONE** (14/21 generated — see log) |
| W3 | M1 wave-2: host-embedded decoders → schema + shim (approach A) | classify/actions diff 0; all 12 | **DONE** (8 converted — see log; ~14/~810 premise corrected) |
| W4 | M2 remainder: views extraction; per-dex records assessed no-ROI | byte-identical; all 12 | **DONE** (honest shortfall — see log) |
| W5 | M5 chain-walk construct | — | **CLOSED: NOT BUILT** (verdict in log) + open_ref dedup landed |
| W6 | M3a language (G-LC map + G-Q any/all both runtimes) + M4 spec moves | vectors 115 both paths; 196 diff 0; all 12 | **DONE** (2 moves landed, 4 parked w/ evidence — see log) |
| W7 | M3b: parse-as-expression + IR v1.1 (minimal, strict bump; fault→reject semantics); then DEX-core shrink pass (attempt all, keep diff-0, park resisters); abort rule per §0.5(d) | full set + regen | PENDING (FULLY PRE-AUTHORIZED §0.5, runs LAST, no check-in needed) |

Execution notes for whoever runs a wave:
- Opus teammate per wave, caveman preamble, hard-rules block (§0.5), RETURN spec, lead
  verifies fresh + commits. Teammates NEVER commit.
- W2/W3/W5 change generated-parser surface → regenerate twins/cpp_surface.json via
  --surface and keep test_cpp_surface green.
- W6 artifact regen → emit via manifest; byte-repro test updates; gen tables re-pair.
- Branch: all waves on `mch-minify`. mch-engine-v2 stays the shadow/flip branch; merge
  decision after program + shadow both conclude (user call).

## 5. Wave log (append after each wave)

- **W6 DONE 2026-07-12** (a: 2707ac30, b: 80adda0b, c: see commit; lead-verified each —
  final: 115/115 vectors both paths, classify/actions 196 A/B 0 AND diff-0 vs pre-W6c
  baseline = true semantic no-op, pytest 1977/2). LANGUAGE: mapc/quant landed end-to-end
  (frontend + py IR + C++ interp/codegen, shared rt cores, 18 new vectors; binder = one
  resolver scope frame — §3.4 soft spot held; R024 no-nesting; `as`-keyword disambiguation
  from the EXISTING map(xs,"field") builtin). SPEC MOVES: 2 landed both-sides-deleted —
  jvault_account_list → `map(... as jw => account(jw))` (corpus 1×) and no_internal_transfer
  → inline `where (not .has_internal_transfer)` (corpus 121×). 4 PARKED with evidence
  (§3.7 class, do not re-attempt without new machinery): tonstakers_minted_nft (EMPIRICAL:
  two-way parity test failed — NFT is an nft_mint BLOCK by tonstakers' turn, op-capture
  can't bind; btype-capture would diverge consumed set), dedust_swap_failed (needs a
  `children(block, op)` topology accessor + include-set membership — NOT in language; G-Q
  landed ZERO production moves, conformance-only for now; children() = candidate future
  feature IF a 2nd use appears), getgems (finish/stop-branch payout = unconsumed-sibling
  read, untested shape → latent silent divergence), tgbtc_mint (list bindings under
  $mint_chain recursion — spec documents they can't be read). Net W6: language capability
  permanent, immediate deletion modest — jvault fn + pred both sides + the moved logic.

- **W6-PRE 2026-07-12** (committed 93e22880): gating verification DONE — new expr kinds are
  ADDITIVE within IR v1.0 (emitter + both loaders regenerate lockstep in-repo; extending
  EXPR_KINDS + runtime dispatch same release = sanctioned path, same as list/record; NO
  version bump; W7/v1.1 stays separate). LOAD-BEARING: `map(xs, "field")` BUILTIN ALREADY
  EXISTS (field projection) — the lambda form `map(xs as e => expr)` is a NEW node kind,
  parser disambiguates on `as`, do NOT overload the builtin. Pinned: null xs→null, non-list
  →fault, empty→[]/false/true, no nested comprehensions, binder scope = body-only (resolver
  scope-frame; STOP if more needed). Executor (minify-w1) handed off at the pre-W6a
  boundary after 5 waves (responsible fatigue call); full executable spec =
  **docs/mch-w6-handoff.md**; minify-w1 TaskStop'd BEFORE spawning fresh executor (per the
  teammate-ops lesson). Fresh session: read handoff doc + this doc §0/§4, execute W6a→c.

- **W5 CLOSED 2026-07-12** (lead-verified: actions A/B 0, prod 0, pytest 1954/2).
  **M5 chain construct NOT BUILT — verdict: not-worth-building** (same class as W4
  per-dex records). The 3 walks are irreducibly heterogeneous: stonfi_v2 pool_accounts =
  nested cross-ref continuation (2 refs deep) + sum_type-set termination + struct-seeded;
  tonco collect_payload_targets = soft-swallow error policy + opcode-gated continuation +
  conditional pair-push + depth-64 recursion (A/B-pinned cycle semantics); only dedust
  steps is linear — and it's single-use AND a mid-fn slice walk (not a message body, would
  need a standalone-walker generator mode for one consumer). A construct covering all 3 =
  every parameter has exactly one consumer = config-shaped meta-generator. All 3 stay hand
  (§3.7 class). maybe_ref-DESCEND (W3 flag) got no 2nd use → stays hand. LANDED instead:
  `open_ref_cell` in PSlice — the byte-identical abort-safe ref-open helper triplicated
  across all 3 walk sites (stonfi ×7, tonco ×4, dedust ×1 call sites; 3 defs → 1, net −6).

- **W4 DONE 2026-07-12** (lead-verified: classify/actions A/B 0, dexrecords-test PASS,
  pytest 1954/2). HONEST SHORTFALL, calibrate future estimates: §3.3's "−800 +450" was
  optimistic — DEX cores assemble legs from dex-COMPUTED values in dex-specific field order
  (W1 finding), so TransferLeg/SwapRecord/per-dex encoders get NO reuse beyond coffee +
  stonfi_v2 jetton arms; building per-dex encoders would relocate field names at ~0 net LOC
  with real byte-risk — SKIPPED by judgment. The genuinely shared M2 surface = DexPton +
  views (data_truthy ×5, acc_str ~16 uses across stonfi+tonco — 2 local copies deleted,
  lt-sort, calls) + TransferLeg::from_jetton_transfer (+2 uses) + coffee adapter.
  HostStonfi −30 (views conversion; cores NOT restructured — failure-fallback derivation =
  low ROI at byte-risk), HostTonco −4 (acc_str), Dedust/DedustDeposit/Nft/Getgems/Tgbtc
  SKIPPED (computed-value legs / non-leg-assembly / W6-deletes-them). M2's real value
  landed in W1 (records+adapter+pton for coffee) + W4 (views); per-dex-record ambition
  does not pay off — recorded so nobody re-attempts.

- **W3 DONE 2026-07-12** (lead-verified: parse/classify/actions A/B 0, msg-vectors 44/44,
  pytest 1954/2). APPROACH A (shim) chosen over full-Value rewrite — B would fight W4's
  typed direction; shim structs = the seam W4 absorbs. 8 decoders converted (CoffeeSwapEvent,
  StonfiPaymentRequest [feeds exit-code pred], StonfiSwapMessage/v1, StonfiV2PayTo,
  DedustDepositLiquidityToPool, NftOwnershipAssignedPrevOwner, JVaultStakePeriodPayload,
  ToncoRouterV3SwapSourceWallet). STAY-HAND with reasons: TgBTCMintEvent (mixed field
  policy + ZERO corpus coverage = byte-identity unverifiable — gate-discipline skip),
  NftTransfer body + telegram Ownership FULL (copy() ref-reset / soft-partial — irreducible
  class), StonfiSwapV2/tonco v3_pay_to/dedust final-partial assets/chain-walks → W5
  (v3_pay_to needs a single-use maybe_ref-DESCEND construct — NOT added, flag if 2nd use
  appears in W5). New DSL constructs (≥2 uses): u32, bits N, _dropped, maybe_ref_cell.
  Generator bug fixed pre-ship (dropped ref_cell didn't consume its ref — fetch now always
  side-effect). "~14 convertible/~810 LOC" premise CORRECTED → 8 clean + irregular class +
  W5 split; host cursor-walks −162 net, +97 schema, consumption LOC falls in W4.
  parse-dump verified opcode-gated (kDumpTypes) — no growth; surface +8 additive
  (runnability keys only on artifact parse types — verified no coupling).

- **W2 DONE 2026-07-12** (lead-verified: parse-dump A/B 0 [577], actions A/B 0, msg-vectors
  28/28, pytest 1954/2 [+4 test_msgs_gen]). Landed: mch-msgs.schema (145 lines, grammar in
  header = the doc) + msgs_gen.py (431, --check repro guard) → MsgParsersGenerated.{cpp,h}
  (562 derived) + ir/msg_vectors.json (boundary vectors: minimal-valid must-parse +
  truncation must-fail; NOT a value oracle — twins own value parity) + --msg-vectors mode +
  ctest mch_msg_vectors (7/7 total). AUDIT CORRECTIONS to §3.1 (doc was optimistic):
  real coverage = **14/21 generated** not 16 — leaf vocabulary is ~10 constructs
  (u64, uint256, coins, addr_py, asset2, asset4, ref_cell, dict_keys267,
  fwd_cell_bits/fwd_cell_refs [remainder keyed on bits vs refs — genuinely different,
  param'd], ref{} descend-flatten); 2 BESPOKE single-use stay hand (MinterJettonMint
  nested-ref-capture-with-prefix; EvaaWithdrawFailExcess opcode-IS-enum) per the program's
  own no-single-use-seams principle; 5 irregulars unchanged (jetton_transfer tail-hook
  candidate for W3+, burn_notification parse_never, dns, vesting, layerzero). The 5 jetton
  regulars were tlbc-based, converted to generated cursor code (var_uint16==load_coins_py,
  load_address_py both paths — proven byte-identical); mch-msgs.tlb STAYS (jetton_transfer
  + minter), 5 tlb constructors dead-but-present. Descriptor table lives Python-side
  (drives vectors); NOT surfaced via --surface (kept cpp_surface.json stable — registered
  set identical). Hand-maintained parse surface ~480 → ~145 schema lines. Deleted:
  ParsePton/ParseStonfi/ParseCoffee/ParseJvault.cpp; shrunk: ParseJettons 283→200,
  ParseDedust 136→57, ParseEvaa 90→46, Parsers.h 55→36. W3 note: host-embedded decoders
  reuse asset2/asset4/ref/fwd_cell (DSL covers); bespoke patterns will recur there.

- **W1 DONE 2026-07-12** (lead-verified: classify/actions A/B 0, vectors 97, build-vec 35,
  dexrecords-test PASS, pytest 1950/2). Landed: DexPton.{h,cpp} (ONE flag drives both
  correlated conventions: pton_conversion=true (v2/tonco) = pton→TON + absent-jetton→Null;
  false (v1) = no conversion + absent-jetton→Asset(TON); reproduces all 3 old variants,
  both dup sites converted), DexRecords.{h,cpp} (SwapLeg/TransferLeg{+from_jetton_transfer}/
  PeerSwap/SwapRecord{fill_peer_out_assets — const_cast KILLED}), HostAdapter.{h,cpp}
  (ConsumedBlocks/HostContext/HostResult<T>/decode_consumed/run_swap_host — coffee thunk =
  one line), BlockViews.{h,cpp} (block_in/first_call/all_calls/first_next_call/
  unique_lt_sorted), DexRecordsTest + --dexrecords-test + ctest mch_dexrecords (6/6).
  HostCoffee −54, HostStonfi −29, HostTonco −27.
  **W4-RELEVANT FINDING: "one SwapRecord encoder for 5 dexes" too optimistic — top-record
  field SET/ORDER is per-dex** (coffee: source_asset early/failed last; stonfi_v1:
  receiver/destination_wallet/success, NO source_asset; stonfi_v2: failed FIRST); leg
  orders also vary (stonfi_v1 OUTGOING reorders dest tail; stonfi_v2 peer inner leg
  {amount,asset} reversed vs coffee SwapLeg). Byte-identical shared surface =
  TransferLeg::from_jetton_transfer + DexPton + views; record/leg ORDER needs per-dex
  encode or field-order lists in W4. data_truthy view deferred to W4 (no coffee use).
