# W7 (parse-expression + IR v1.1 + DEX shrink) — handoff spec (§8a pattern)

**Status:** W7a DONE + lead-verified/committed boundary. W7b/W7c PENDING. Handed off
here (post-W7a) per the lead's context-aware stop option — same pattern minify-w1 used
pre-W6. Branch `mch-minify` (verify `git branch --show-current` = mch-minify before ANY edit).
Fully PRE-AUTHORIZED by user (program §0.5 (a)-(d)); NO further check-in — the §0.5 decisions
ARE the answers. Sub-stage reports to team-lead + lead commits each (W7b then W7c).

## 0. What W7a already landed (Python parse-expression, IR still v1.0)

`parse <target> as T (| T)*` as a build EXPRESSION (distinct from the `parse CAPTURE as T`
STATEMENT). Failure semantics = **FAULT → build reject** (null/non-block target OR no type
parses); NO soft-null variant (§0.5(a)). Stateless — returns the parsed message object
directly (no `bodies` side table), which "fixes the hidden-mutable-state parse model" (§3.4).

Files (frontend + py IR, all committed by lead at the W7a boundary):
- `ast_.ParseExpr(target, msg_types)`.
- `parser.py` `_parse_parse_expr` + hook in `_parse_primary` (the STATEMENT form is consumed by
  `_parse_build_stmts` before expression parsing, so a `parse` in expr position is always the
  expr form). Target = `_parse_postfix` (capture / element var / `.field` chain); `as` terminates.
- `resolver.py`: `_check_expr_refs`/`_check_out_fields` now take `registries`; ParseExpr recurses
  target + validates `msg_types` (R012). (All call sites already threaded.)
- `expr_ir.py`: emits `{"k":"parse","x":<expr>,"types":[...]}`.
- `mch_ir/loader.py`: `EXPR_KINDS += "parse"`; `_check_expr` parse structure; `_check_where_expr_sync`
  rejects parse.
- `mch_ir/expr.py`: async `_eval_parse` — eval target → block, `block.get_body()`, try each type
  (first success wins), fault on total failure / null / non-block / unregistered type.
- `compiler.py`: `_check_where_expr_sync` rejects ParseExpr (C013).
- Spec: `mch-ir-spec.md` §7 vocab + shape + new §7.4.2 + load-check list + §7.5.
- Tests: `tests/mch_ir/test_parse_expr.py` (11: eval success/fault/alternatives/inside-map + loader
  accept/reject/where-reject + resolver R012). pytest = **1988 passed / 2 skipped**.

IR version DELIBERATELY still "1.0" in W7a; `build_vectors.json` and the production
`ir/mch_ir.json` UNTOUCHED (no spec uses parse-expr yet → W7c; C++ build stays green). C++ gates
NOT run in W7a (parse kind unknown to C++ until W7b — expected, like W6a).

## 1. W7b — C++ parse-expression + STRICT IR v1.1 bump (ONE commit)

### 1a. C++ parse-expression (the real design point)
The STATEMENT form's runtime is `rt_parse(env, target, types)` (BuildRuntime.cpp:51): it calls
`soft_parse(block, types, &body)` which yields the parsed body as a **Value** (an Obj), then
STORES it in `env.bodies[block]` and returns Null; `.body` reads it back via `rt_access_build`.
On total failure `soft_parse` leaves `body` Null (the statement's soft semantics).

The EXPRESSION form must instead **return the body Value directly and FAULT on total failure**:
- Add `EvalResult rt_parse_expr(BuildEnv &env, const Value &target, const std::vector<std::string> &types)`
  in BuildRuntime.{h,cpp}: null/non-block target → fault; call `soft_parse`; if the result body is
  Null (all types failed) → **fault** (NOT soft-null); else `rt_ok(body)`. (Reuse `soft_parse`;
  just invert the total-failure outcome.) The returned Obj's fields are read by `attr`/`rt_access_build`.
- `ExprEval.cpp` interpreter: add a `k=="parse"` case. NOTE the interpreter has NO BuildEnv (it's
  Env/Lookups/WhereEnv). Parse is build-only. So: in the interpreter, `parse` should fault UNLESS a
  build context is available — but the interpreter is only used for --vectors (value-only, no blocks)
  and where (sync). So the interpreter `parse` case = defensive `rt_fault` (parse is exercised only
  through the generated BUILD path + the py evaluator). Mirror how the interpreter has no host-fn
  execution. (Confirm: --vectors never contains a parse node; build_vectors go through the generated
  BuildGen path, not ExprEval. So interpreter parse = fault is correct + gated.)
- `codegen/ExprCodegen.cpp`: add `k=="parse"` to `Emitter::emit`. Build mode → `bind("rt_parse_expr(env, "
  + emit(x) + ", std::vector<std::string>{" + cstr-joined types + "})")`. Where/Vectors mode →
  defensive `rt_fault` (like the lookup where-guard). Emit the types as a `std::vector<std::string>{...}`
  of `cstr(type)`.
- `IrLoader.cpp`: no EXPR_KINDS validator exists (defers to interp-fault/codegen-throw — the W6b
  design note the lead accepted); "acceptance" = ExprEval + codegen handle "parse". No structural
  validator to extend. (Same as mapc/quant in W6b.)

### 1b. STRICT IR v1.1 bump (§0.5(b): minimal, no dual-accept, ONE commit)
Bump every `mch_ir_version`/`IR_VERSION` "1.0" → "1.1" so a 1.0 loader rejects a 1.1 artifact and
vice-versa. Surface (grep-verified): `indexer/events/mch/ir_emit.py` (IR_VERSION via expr_ir),
`indexer/events/mch/expr_ir.py` (IR_VERSION="1.0"), `indexer/events/mch_ir/loader.py` (IR_VERSION="1.0"),
`ton-index-worker/ton-mch-engine/src/IrLoader.cpp` (IR_VERSION), and fixtures in
`tests/mch_ir/test_loader.py` (version-string cases). Then:
- Regen the production artifact via the manifest ONCE (`python -m indexer.events.mch.ir_emit --manifest
  ir/emit_manifest.json -o ir/mch_ir.json`) — it flips to "1.1" (still no parse nodes; that's W7c).
- Regen generated tables (automatic on C++ build; sha-paired).
- Update the byte-repro guard (`test_ir_emit.py`) + any fixture asserting "1.0".
- Add a **parse build-vector** to `ir/build_vectors.json` (both paths): model on the existing
  parse-STATEMENT build-vector (block env with `body_b64`, a real registered message type). Cover
  success (parsed field in `out`) AND fault (a body no type parses → the case's `expect` = reject).
  Bump `build_vectors_version` if it carries one. This is what gates parse both-paths.

### 1c. W7b gate
build-vectors N/N BOTH paths + full 12 (match 328 / classify 196 / actions 196 all A/B 0, schema+prod
0, parse-dump 577, msg-vectors 44, ctest, expr-vectors 115×2) + pytest + 0 aborts. Everything stays
196/diff-0 (the artifact only changed its version tag + regenerated identically otherwise).

## 2. W7c — DEX-core shrink pass (§0.5(c): attempt all, keep diff-0, park resisters)

CANDIDATES = the **hop-loop / per-hop leg record assembly** inside the host fns
`stonfi_v2_swap_data`, `tonco_swap_data`, and the dedust legs core (`_dedust_swap_legs`). Attempt
moving the per-hop record building into the spec `out{}` using **map + parse-expression**:
`peer_swaps: map(swap_requests as s => { in: {...parse s...}, out: {...} })` shape. The captures for
the hop lists already exist in the specs (kept "for pattern fidelity"); the blocker was per-element
parse, which parse-expression now provides.

RULES:
- Keep ONLY moves where classify/actions stay **196 A/B diff 0 AND diff 0 vs pre-move baseline**
  (snapshot `--classify`/`--actions` BEFORE each move; the W6c method). ANY delta = revert that move,
  park with evidence. Do NOT tweak toward green.
- These STAY PARKED (their blockers are NOT parse-expression, verified in W6c): `tgbtc_mint_data`
  (list-binding-under-recursion captures), `getgems_nft_purchase_data` (unconsumed-sibling payout),
  `tonstakers_minted_nft` (nft_mint block can't be op-captured w/o consuming), `dedust_swap_failed`
  (needs a `children` block-topology accessor). Don't re-attempt.
- The DEX cores also have irreducible async residue (multi-hop pool-account resolution / jetton-wallet
  lookups / failure-fallback derivation — §3.7 "stays hand"). Expect PARTIAL shrink at best; the cores
  likely stay HYBRID. Report corpus-verified vs pytest-only per move (stonfi_v2/dedust fire in corpus;
  check tonco).
- ABORT (§0.5(d)): if the resolver needs capture-scoping semantics beyond W6's single scope-frame
  (e.g. parse-result-record fields acting as capture-like bindings that complicate scoping), PARK the
  remainder, ship what's green, and report the scoping design as a PROPOSAL (do not build it).

## 3. Gate commands / paths (proven W1-W6)
`EXE = ton-index-worker/cmake-build-debug-visual-studio/ton-mch-engine/ton-mch-engine.exe`,
`ENG = ton-index-worker/ton-mch-engine`, `ART = ENG/ir/mch_ir.json`, `TR = indexer/tests/traces`,
`PY = S:\Dev\H-dump\tonscan-indexator\venv0\Scripts\python.exe`, twins in `ENG/twins/` run from
`indexer/`. Build: PowerShell tool + `ENG/twins/build_mch.bat` with `$env:CMAKE_BUILD_PARALLEL_LEVEL=4`;
CMakeLists change (adding rt_parse_expr does NOT change CMakeLists — it's in an existing TU) → none
expected, so no reconfigure. A/B = `Compare-Object`/diff engine-stdout vs the matching `twins/py_*`
(0 lines). Surface regen if the host registry changes (W7c may delete host fns): `EXE --surface >
ENG/twins/cpp_surface.json`, keep `tests/mch/test_cpp_surface.py` green.

## 4. Hard rules (unchanged)
Branch `mch-minify` only; -j4; PowerShell + build_mch.bat (NEVER Bash for C++); Write/Edit only for
C++ (no heredocs); no aborts (throw catchable); no Value.h layout change; NEVER git commit/push/checkout
(lead commits per sub-stage); Python = ground truth; never break a green gate; pre-existing working-tree
changes (core/database.py, core/settings.py, serializer hunks, tgbtc.yaml, deleted lz4, ton-index-go,
submodule ptrs) stay OUT.
