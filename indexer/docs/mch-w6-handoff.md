# W6 (language wave) — handoff spec (§8a pattern)

**Status:** pre-W6a boundary. Gating verification DONE (below). Implementation NOT started.
Handed off per the lead's context-aware stop option (long session; W6a is a delicate
new-surface feature). Branch `mch-minify`, clean (W1–W5 committed through 412edd73).

## 0. Gating verification (the lead's pre-build ask) — RESULT: additive within v1.0, NO version bump

Question: do new expr node kinds (`map`-lambda / `any` / `all`) require an IR version story?

Findings (verified in code):
- The py loader IS fail-closed on expr kinds: `mch_ir/loader.py:214` — `_check_expr` raises
  `IrLoadError` if `e.get("k") not in EXPR_KINDS`. `EXPR_KINDS` is a frozenset at `loader.py:22`.
- The py runtime IS fail-closed: `mch_ir/expr.py:97` — `eval` ends `raise EvalError("unsupported
  expression kind {k}")`.
- Node kinds (§5) and expr kinds (§7) are SEPARATE checks; expr uses EXPR_KINDS.

Conclusion: **additive within `mch_ir_version` "1.0"`, no bump.** The version tag is an
emitter↔loader compatibility contract; emitter + both loaders are always regenerated in
lockstep in-repo (no external "1.0" consumer), so extending EXPR_KINDS + runtime dispatch in the
same release is the sanctioned additive path (identical to how `list`/`record` were added). The
fail-closed check does NOT force a bump — it forces both loaders to learn the kind in their
sub-stage (py in W6a, C++ in W6b). parse-as-expression / IR v1.1 stays SEPARATE (W7).

Between W6a and W6b the C++ loader will reject/fault the new kinds — that is the lead's expected
"C++ vectors FAIL in W6a"; do NOT run C++ gates in W6a. New kinds go ONLY into
`ir/expr_vectors.json`, never the production `ir/mch_ir.json` in W6a.

## 1. Load-bearing collision finding

`map` is ALREADY a builtin: `map(xs, "field")` = field-projection, string-literal arg
(`mch-spec.md:617`, IR spec §7.3 builtin table). The NEW `map(xs as e => expr)` is a DIFFERENT
construct (lambda comprehension). The parser MUST disambiguate by the `as` keyword:
`map(xs, "f")` → existing builtin `call` node; `map(xs as e => expr)` → NEW comprehension kind.
Do NOT overload the builtin. `any`/`all` are new names (no collision).

## 2. Pinned design (lead + spec §16.1)

- G-LC `map(xs as e => expr)`: ONE bound element var `e`, ONE pure sub-expr; NO nested
  comprehensions (spec §16.1 "no nested comprehensions" — enforce in resolver/parser, document).
  C++ = std::transform. Result: a list.
- G-Q `any(xs as e => bool)` / `all(...)`: quantifier, per-element bool predicate. C++ =
  std::any_of / std::all_of. Result: bool.
- Binder scope: `e` is visible ONLY inside the lambda body (new scope frame). THIS IS THE
  §3.4 SOFT SPOT — if binding a captured list under recursion needs more than a new-scope-frame
  in the resolver, STOP and report options (program abort-rule analog).
- null xs → **null** (match existing list-builtin short-circuit; `mch-spec.md:617` "a null list
  argument yields null"). Pin in spec + a vector.
- xs not a list (non-null, non-list) → **fault** (existing list combinators fault on non-list;
  verify against the `first`/`map` cores and match). Pin + vector.
- empty list → map=`[]`, any=`false`, all=`true` (conventional). Pin in spec + vectors.
- binder shadowing: `e` shadowing an outer capture inside the body only — vector for it.

## 3. File map

Frontend (`indexer/events/mch/`):
- `tokens.py` — `as`/`=>` tokens if not present; `map`/`any`/`all` stay identifiers (contextual).
- `parser.py` — parse `NAME(expr as IDENT => expr)` for map/any/all; `map(expr, str)` stays the
  builtin call. Disambiguate on `as`.
- `ast_.py` — new AST node(s): e.g. `Comprehension(kind, xs, var, body)` (kind ∈ map/any/all).
- `resolver.py` — bind `var` in a NEW scope frame for the body ONLY (THE soft spot). Reject
  nested comprehensions; reject `var` colliding illegally if the resolver rules require.
- `expr_ir.py` — emit new node(s): pin a shape, e.g. `{"k":"mapc","xs":…,"var":"e","body":…}`
  and `{"k":"quant","op":"any|all","xs":…,"var":"e","body":…}`. (Choose `k` names; add to spec §7.)
- `compiler.py` — if it walks expr trees, handle the new nodes.

IR + runtime (py):
- `mch_ir/loader.py` — add new `k` to `EXPR_KINDS` (line 22) + structure validation in
  `_check_expr` (recurse into `xs`+`body`; `var` is a string).
- `mch_ir/expr.py` — `eval` dispatch: build the element env (bind `var`), eval body per element;
  map→list, any→any_of, all→all_of. Respect null-xs→null, non-list→fault, empty conventions.
- `mch/expr_eval.py` — the SYNC where-evaluator + shared cores. map/any/all are BUILD-expr
  features; where_expr is builtin-only sync (IR spec §7.4). Likely NOT allowed in where_expr →
  keep them OUT of the sync path (and `_check_where_expr_sync` should reject them, like `lookup`).
  Verify: do any shared cores in expr_eval need the new kinds? Probably no.

Spec docs:
- `mch-spec.md` §16.1 — move DEFERRED → v1.0-ADOPTED note (keep the design text; add the pinned
  null/empty/nesting rules).
- `mch-ir-spec.md` §7 — add the new `k` to the vocabulary list + a JSON shape example + §7.3/§7.4
  notes (not builtin-null-strict; not sync-where-eval-able).

Vectors:
- `ir/expr_vectors.json` — grow with: basic map (record-literal body), map over a captured list,
  any true/false/empty, all true/false/empty, null xs → null, non-list xs → $fault, binder
  shadowing. (See `tests/mch_ir/vectors/_generate.py` + `test_expr_vectors.py` for the format;
  vectors are ALSO run C++-side in W6b.)

C++ (W6b, `ton-index-worker/ton-mch-engine/src/`):
- `IrLoader.cpp` — the EXPR_KINDS-equivalent accept + structure check.
- `ExprEval.cpp` + `ExprRuntime.{h,cpp}` — one-semantics: put the map/any/all core in ExprRuntime
  (a `rt_*`), called by BOTH the interpreter (ExprEval) and codegen. Element env binding in the
  interpreter mirrors the py env.
- `codegen/ExprCodegen.cpp` — emit std::transform / any_of / all_of over the element with a
  generated lambda that binds `var`.
- `Value.h` — no new VType expected (result is List/Bool). Verify no ABI change (if a field is
  added, the CLEAN rebuild rule from memory `mch_cpp_value_abi_rebuild` applies).

## 4. Sub-stage gates (commands proven across W1–W5)

Paths: `EXE = ton-index-worker/cmake-build-debug-visual-studio/ton-mch-engine/ton-mch-engine.exe`,
`ART = ton-index-worker/ton-mch-engine/ir/mch_ir.json`, `EV = …/ir/expr_vectors.json`,
`TR = indexer/tests/traces`, `PY = S:\Dev\H-dump\tonscan-indexator\venv0\Scripts\python.exe`,
twins in `…/ton-mch-engine/twins/` run from `indexer/`. Build: `twins/build_mch.bat` with
`$env:CMAKE_BUILD_PARALLEL_LEVEL=4`; CMakeLists change → plain-env CLion-cmake reconfigure first.

- **W6a gate** (py only): `PY -m pytest tests/mch tests/mch_ir tests/test_actions.py -q` green;
  py-side expr-vector runner green on the grown vectors (see test_expr_vectors.py). Report the new
  vector count. Do NOT run C++ (`EXE --vectors`) — expected to fail (kind unknown until W6b).
- **W6b gate**: `EXE --vectors EV` and `EXE --vectors-gen EV` both N/N; full 12 (parse/match 328/
  classify 196/actions 196/schema C+A/prod C+A all diff 0 vs twins); ctest 7/7; msg-vectors 44/44;
  pytest green; 0 aborts.
- **W6c gate** (M4 spec moves): CRITICAL — classify/actions stay **196 diff 0** (semantic no-ops);
  ANY delta = STOP. Regen artifact via `PY -m indexer.events.mch.ir_emit --manifest …emit_manifest.json`
  ONCE; regen `twins/cpp_surface.json` via `EXE --surface`; byte-repro test updates
  (test_ir_emit + test_msgs_gen patterns); delete moved fns BOTH sides (C++ host TU + py twin
  surface via cpp_surface). Report which moves are corpus-verified vs pytest-only (tgbtc/getgems
  have ~zero corpus fire → pytest-conformance-gated only).

## 5. W6c move list (program §3.5)

- `tonstakers_minted_nft` → optional-child capture (opcode 0x132F9A45) + `.msg.destination`.
- `getgems_nft_purchase_data` → spec `out{}` (18 let/field copies; payout pred exists). pytest-only.
- `tgbtc_mint_data` → spec `out{}`; KEEP `bitcoin_txid`/txid_le_hex fn. pytest-only.
- `jvault_account_list` → G-LC (`map(xs as e => account(e))`) — its own comment says it exists only
  because G-LC was deferred.
- `dedust_swap_failed` → G-Q (`any(swaps as s => not any(children(s, notif)))` — verify the exact
  predicate against the host fn).
- `no_internal_transfer` → inline `where` on `.data` IF the resolver permits (verify; else stays).
  NOTE W4 turned this into `data_truthy` (BlockViews); the spec-move deletes the host fn if the
  inline-where lands.
- Delete each moved fn from `HostRegistry.cpp` + its host TU + the twin surface; the maybe_ref-
  DESCEND (tonco v3_pay_to) has NO 2nd use and stays hand.

## 6. Hard rules (unchanged)

Branch `mch-minify` only; -j4; PowerShell + build_mch.bat (never Bash for C++); no aborts (throw
catchable); Python is ground truth; never break a green gate; teammate NEVER commits (lead
verifies + commits per sub-stage). Program principle: no single-use DSL constructs (map/any/all
each earn ≥2 uses via W6c moves — that's what justifies the language wave).
