# MCH C++ Engine — Phase G Production Design (DRAFT for user review)

Status: DRAFT 2026-07-11, written after B–F completion (all gates green at db31c3ad:
classify/actions A/B diff 0 both artifacts, 196 rows; pytest 1950/2). Companion to
`mch-cpp-plan.md` §9. NOTHING here is implemented; every numbered decision needs user
sign-off (recommendations marked ►).

## 0. What Phase G is

Turn the gate-proven offline engine (`ton-mch-engine` CLI + fixture lookups + dump modes)
into a classification stage inside the production C++ indexer (`ton-index-postgres`), writing
`actions` rows. The parity bar stays: same Action rows the Python classifier would write
(minus the deliberate deltas listed in §6).

## 1. Integration point

`IndexScheduler` pipeline today: fetch → parse → trace_assemble → interfaces → actions →
insert, where "actions" is currently served by the Python event-classifier reading
`_classifier_tasks`. Phase G replaces that seam for MCH-classifiable traces:

- New library target `mch-classify` (engine minus CLI/dump/harness code) linked into
  ton-index-postgres. `main.cpp` stays a dev tool.
- A `MchClassifierActor` receives assembled traces (+ their interface data) after the
  interfaces stage, produces Action rows, hands them to `InsertBatchPostgres`.
- ► Recommendation: per-trace actor requests batched per mc-seqno, mirroring the existing
  stage actors' shape. The engine itself stays synchronous per trace (two-phase lookups make
  it a pure function of trace + lookup table — see §3).

Open question Q1 (user): does Python classification keep running in parallel during rollout
(shadow mode: C++ writes to a compare table / logs diffs, Python stays authoritative), or is
the flip per-matcher-set with Python fallback for non-MCH traces? ► Recommend shadow mode
first on a bounded seqno range; it reuses the A/B discipline we already trust.

## 2. Harness/runtime isolation (prerequisite refactor = plan §11 item 2)

Before embedding, split the engine so the library has no harness residue (this also resolves
review findings §4.3/§6.3, deferred to exactly here):

- `Classify.cpp` → `ClassifyCore` (trace + LoadedIr + LookupSource + registries →
  `ClassifyResult { actions, skips, failure }`) and `ClassifyDump` (CLI rendering over the
  result — the current stdout format becomes an adapter, twins unchanged).
- Buffered, atomic per-trace results: actions are emitted only from a committed result;
  a mid-trace throw yields `failure` with NOTHING emitted (replaces the current
  streamed-then-TRACE_FAIL dump behavior; the dump adapter reproduces today's stream for the
  A/B twins so gates stay byte-stable).
- run_parse_dump leaves MsgParse; block_key/rt_eq cleanups per §11 item 2 ride along.

## 3. Production LookupSource

The engine's `LookupSource { fetch(kind, key) }` gets a production adapter:

- Interface data source: the pipeline's interfaces stage already computes jetton wallets/
  masters, NFT items/collections/sales, etc. for the trace's accounts — the same data the
  fixture `interfaces` section snapshots. Adapter maps lookup kinds to those in-memory
  structs; missing → null (semantics already pinned).
- Two-phase stays: collect pass gathers keys → adapter resolves from the interfaces stage
  output (in-memory; no extra DB round expected) → resume. If a kind ever needs DB access
  not in the interfaces output, it becomes an actor request round — the seam supports it,
  nothing else changes.
- Computed lookups (jvault_assets pattern, review §9.3): stay named resolvers behind the
  same seam; production impl reads the same interface structs.
- Q2 (user): confirm the interfaces stage output is available to the actions stage in
  ton-index-postgres today (Python reads it from DB/kvrocks; C++ in-process handoff would be
  new plumbing). If not in-process, the adapter's fetch goes through the existing storage.

## 4. Artifact + registries at build time

- IR artifact embedded at build (no hot reload — standing decision): current
  ir/mch_ir.json + mch-codegen generated tables, sha-paired (mechanism exists and is CMake-
  integrated). ► Keep the runtime JSON loader too (dev/tests); production init prefers the
  embedded blob.
- Host bindings (preds/fns/shapers/parsers/lookup kinds) are compiled in; the loader's
  registry-gated runnability report becomes a STARTUP log: any artifact matcher that isn't
  runnable in production is a loud warning (config error), not a silent skip. Q3 (user):
  should unrunnable-matcher-at-startup be fatal instead? ► Recommend fatal in production
  builds — the artifact and binary ship together, a mismatch is a build bug.

## 5. Action persistence

- ClassifyResult.actions → `InsertBatchPostgres` actions insert (ON CONFLICT DO NOTHING,
  same idempotency as Python).
- Columns: the ported 33-column set + composite types already A/B-proven at the row level.
  Missing vs Python today (deliberate, staged): see §6.
- Cleanup-on-reprocess (delete actions/action_accounts by trace_id) must be reproduced for
  the C++ path when it owns a trace.

## 6. Known deltas to close (or bless) before flip — the Phase G worklist

1. accounts + role_assignment + action_accounts rows — NOT ported (needs
   initiating_event_node on the C++ Block + role logic port). REQUIRED before flip:
   action_accounts feeds the API's account queries.
2. tx_hashes extension by initiating_event_node — same member, same wave as (1).
3. unwind_deployments post-processor — absent both sides in the harness; production Python
   RUNS it. Must be ported before flip (small tree pass).
4. nft_purchase C++ fill — blocked on corpus coverage; before flip either port blind with
   Python-fixture unit tests or record a purchase trace (user call — standing no-recording
   rule was for matcher waves).
5. Basic-actions fallback: Python's try_classify_basic_actions on trace failure
   (ton_transfer/call_contract/contract_deploy only). Port or route failed traces to Python.
   ► Port — it's small and keeps the C++ path self-sufficient.
6. Trace-level roles aggregation + any `_classifier_tasks` bookkeeping the Python worker
   does around a trace (mark done, etc.) — inventory needed.
7. T20 priority ties: production artifact relies on file-name ordering ('a_' prefix hack).
   Assign explicit priorities in the specs before flip (mechanical, artifact regen).
8. BOC bytes: C++ writes td-serialized payload bytes where Python wrote pytoniq bytes
   (root-hash-equal; §12 evidence says no consumer compares). Blessed 2026-07-11 — reindex
   delta note only.

## 7. Rollout sequence (proposed)

1. §2 isolation refactor (library + buffered results) — gates unchanged.
2. §6 items 1/2/3/5 (accounts+roles wave, unwind_deployments, fallback) — extends the
   --actions gate to full-row parity incl. action_accounts.
3. §3 production LookupSource adapter + §4 embedding + §1 actor — first shadow run on a
   dev seqno range, diff vs Python-written rows.
4. §6 item 7 priorities + remaining blessed deltas documented.
5. Benchmark vs Python (dev_utils/mch_bench.py methodology) on the corpus + a live range.
6. Flip decision (user) with the shadow-diff report in hand.

## 8. Explicitly out of scope for Phase G

- Matcher coverage growth (remaining legacy-only matchers) — orthogonal, continues on the
  Python side; C++ picks them up via artifact regen when they go declarative/hybrid.
- Typed-IR/execution-plan consolidation (review Wave 1) — reconsider AFTER the isolation
  refactor lands; do not block production on it.
- tlb/--msgs parser codegen frontend — parser productivity track, separate.
