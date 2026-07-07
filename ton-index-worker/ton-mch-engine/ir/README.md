# ir/ — version-controlled MCH IR contract copies

These JSON files are the cross-language contract consumed by the C++ build
(mch-codegen reads them; no Python is required to build ton-mch-engine).

- `mch_ir.json` — the compiled matcher IR artifact (currently 32 matchers,
  12 with declarative build programs). Input for `--wheres` (where_expr
  functions) and `--builds --suffix ir` (build-program functions used by
  `--classify` / `--build-smoke`); loaded at runtime by `--match` /
  `--build-smoke` / `--classify`.
- `expr_vectors.json` — 91 expression conformance vectors. Input for
  `--vectors` (functions run by `--vectors-gen`); loaded at runtime by
  `--vectors` / `--vectors-gen`.
- `build_vectors.json` — 36 build-program conformance cases. Input for
  `--builds --suffix vectors` (functions run by `--build-vectors`); loaded at
  runtime by `--build-vectors`.

## Codegen is a build step — never regenerate by hand

Every generated source is produced by a CMake custom command into the build
tree (`<binary-dir>/ton-mch-engine/generated/`) and is NOT in git:

| input (git)           | generator                              | output (build tree)           | table symbols                                             |
|-----------------------|----------------------------------------|-------------------------------|-----------------------------------------------------------|
| ir/expr_vectors.json  | mch-codegen --vectors                  | vectors_generated.cpp         | `gen_vectors()`                                           |
| ir/mch_ir.json        | mch-codegen --wheres                   | wheres_generated.cpp          | `gen_wheres()`, `gen_wheres_source_sha()`                 |
| ir/mch_ir.json        | mch-codegen --builds --suffix ir       | builds_ir_generated.cpp       | `gen_builds_ir()`, `gen_builds_ir_source_sha()`           |
| ir/build_vectors.json | mch-codegen --builds --suffix vectors  | builds_vectors_generated.cpp  | `gen_builds_vectors()`, `gen_builds_vectors_source_sha()` |
| tlb/mch-msgs.tlb      | tlbc                                   | mch-msgs-tlb.{cpp,h}          | `mchmsg::gen::*`                                          |

Editing an input JSON (or the .tlb) and rebuilding `ton-mch-engine` is the
whole workflow — CMake reruns the affected generator automatically. The
`--builds` tables carry distinct symbols, so every table links into the one
binary and each runner is permanently paired with its own document; the
runners' sha256 guards remain as a safety net (a mismatch is a hard exit 2,
but with build-step generation it cannot happen unless a stale binary is run
against edited inputs without rebuilding).

## Running against a different artifact

`--classify` / `--build-smoke` / `--match-gen` accept only an artifact whose
bytes match a table baked at build time. Two ways to run another artifact:

- **Contract swap**: overwrite `ir/mch_ir.json` and rebuild (seconds — only
  the affected tables regenerate). Use when the new artifact IS the new
  contract.
- **Testbed table** (`-DMCH_TESTBED_IR=<path/to/artifact.json>` cache var):
  generates a THIRD build table (`gen_builds_testbed()`) from an out-of-tree
  experimental artifact; classify/build-smoke pick the ir or testbed table by
  the loaded artifact's sha automatically. The contract file stays untouched —
  intended for phase work that lives off experimental artifacts. Unset the var
  (default) and the committed empty stub (`src/BuildsTestbedStub.cpp`) is
  compiled instead. Note: the testbed covers the BUILD tables only; `--match`
  (interpreter) already accepts any artifact, but `--match-gen` remains paired
  with `ir/mch_ir.json`.

## Concurrency hazard

Never share one binary dir across concurrently working agents/sessions: a
parallel edit of an input JSON mid-build can be captured by one generator run
and not another (and Syncthing can revert a file with its mtime preserved, so
ninja may not even rerun the generator). The sha guards catch the resulting
mismatch at run time, but the fix is isolation: one binary dir per agent.

## Syncing from the frontend

Source of truth lives in the indexer frontend:

- `mch_ir.json`        <- `python -m indexer.events.mch.ir_emit <specs> -o ...`
- `expr_vectors.json`  <- indexer/tests/mch_ir/vectors/expr_vectors.json
- `build_vectors.json` <- indexer/tests/mch_ir/vectors/ (build-program cases)

Copying the refreshed JSON here is still a manual step; everything after the
copy (regeneration, table pairing) is handled by the build.
