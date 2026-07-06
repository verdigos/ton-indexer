# MCH C++ Build-Program Execution — Design (Phase A)

Status: decisions user-approved 2026-07-07. Companion to `mch-cpp-plan.md` (Phase B implements
this). Ground truth for semantics: `indexer/indexer/events/mch_ir/program.py`.

## Decisions (user-signed)

### D1. Lookup async model: two-phase collect → fetch → resume
- **Collect pass**: the generated build fn runs with a `CollectingLookupTable` — every
  `lookup` records `(kind, key)` and returns null. Statements are pure apart from lookups, so
  the dry run is side-effect-free. Reject/failed short-circuits may end the pass early — keys
  collected up to that point suffice (Python batches the same way: keys unreachable before a
  reject can't be needed).
- **Fetch**: `LookupSource` interface — `Value fetch(kind, key)` batched per trace (actor
  request round in production; fixture-backed table offline — the .lz4 fixtures bundle the
  interface data the Python TestInterfaceRepository uses; a loader for that section is part of
  Phase B2).
- **Resume pass**: same generated fn with the filled table. Determinism: table is immutable
  during the pass; missing key → null (mirror Python nullable-lookup semantics exactly).
- Rejected alternative: C++20 coroutines (toolchain risk, harder deterministic A/B, no
  batching win over two-phase).

### D2. Build statements are CODEGEN'd (no interpreter)
`mch-codegen --builds <mch_ir.json>` emits one `BuildOutcome build_<matcher>(BuildEnv&)` per
matcher carrying a build_program. Statement mapping:
- `let NAME = expr` → C++ local (`Value l_NAME`), no runtime env.
- `parse CAP as MsgType` → `MsgParse` registry call on the captured block's body; soft-parse
  failure behaves EXACTLY like program.py (check its rule per statement use — parsed-object
  null vs reject).
- `reject when expr` → early `return BuildOutcome::rejected()` on true (fault-in-condition
  semantics per program.py).
- `failed when` / `broken when` → flag sets on the outcome.
- `flow ...` → append FlowOp records to the outcome (executed against AccountValueFlow in
  Phase E; until then recorded + dumped for A/B).
- `out { k: expr, k?: expr }` / `produces switch { when e => btype out{...} ... else => ... }`
  → data map (optional keys omitted when null) + btype selection (first-true, else required).
  Execution order, null handling, optional-key rule ported statement-for-statement from
  program.py.
- No statement interpreter is built. If debugging demands one later, that's a new decision.

### D3. Env layout: slot-indexed, extending WhereEnv
`BuildEnv { const Block *anchor; const Value *slots; size_t n_slots; const LookupTable *lookups;
BocLib *boc; ... }` — same shape family as `WhereEnv{block, slots, n_slots}` (signature designed
for this in the where_expr stage). Captures are slot-indexed (the walker already binds slots;
capture NAMES never appear at runtime). List captures = Value lists in their slot. Parsed
bodies and `let` locals are generated C++ locals, not env entries.

### D4. BOC bytes: raw-byte equality is the parity bar
User assessment (verified on flags): pytoniq `to_boc()` defaults = has_idx False,
hash_crc32 False, cache_bits False ≡ td `vm::std_boc_serialize(cell, 0)`. Cross-language
Cell-derived bytes/strings (e.g. vesting `message_boc_str = b64(cell)`) must be BYTE-IDENTICAL.
- The earlier "different ref layouts" finding is suspected self-inflicted (the parse-dump used
  WithCRC32C on the C++ side). **Phase B task B0 verifies**: byte-level BOC dump A/B for every
  payload cell in the corpus (381 jetton messages), mode 0 both sides. Residual diffs are BUGS
  to investigate (first suspects: serialization flags, then cell ordering on shared subtrees) —
  divergence is not blessed. `MsgParse` adapters switch payload bytes to mode 0 accordingly.

## BuildOutcome contract
`{ status: accepted | rejected; btype; data: Value(Dict); failed, broken: bool;
flows: vector<FlowOp> }` — rejected ⇒ walker returns `[]` (clean rejection §10.7: no
consumption, no auto-append, anchor yielded to later matchers). Accepted ⇒ produced Block from
arena (generic Block + dict data, per the canonical-product decision), `merge_blocks(consumed)`
(false ⇒ clean rejection too), THEN root-level excess/bounce auto-append to the RESULT list
(after build — v1 parity).

## A/B strategy (Phase B3 gate)
New `--classify` dump: per fixture, per fired matcher — produced btype, data map (canonical
rendering per the parse-dump conventions), consumed set, failed/broken, flows, auto-appended
blocks. Python twin runs the real IR engine loop. Matcher inclusion is driven by an explicit
artifact-derived skip table (host bindings not yet ported ⇒ SKIP lines, both sides identical);
Phases C/D shrink it. diff 0 over 147 fixtures per included matcher set.
