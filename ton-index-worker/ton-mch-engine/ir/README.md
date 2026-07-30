# ir/ — version-controlled MCH IR contract copies

These JSON files are the cross-language contract consumed by the C++ build
(mch-codegen reads them; no Python is required to build ton-mch-engine).

- `mch_ir.json` — the compiled matcher IR artifact (currently 39 matchers, all
  declarative). A pure BUILD input: `--matchers`, `--wheres` and `--builds
  --suffix ir` compile it into the matcher table, the where functions and the
  build programs, and NOTHING reads it at run time (the `mch_ir_equivalence`
  ctest is the only remaining reader of the document, and it is a build gate).
  The two
  legacy builder-tier jetton specs (specs/jetton_transfer.mch, specs/jettons.mch)
  are excluded from this PRODUCTION artifact (A4) — see the manifest's
  `excluded_note`. They stay in specs/ for the Python conformance suite; emitting
  from the specs/ dir directly (emit_dir) still includes them.
- `expr_vectors.json` — 127 expression conformance vectors. Input for
  `--vectors` (functions run by `--vectors-gen`); loaded at runtime by
  `--vectors` / `--vectors-gen`.
- `build_vectors.json` — 38 build-program conformance cases. Input for
  `--builds --suffix vectors` (functions run by `--build-vectors`); loaded at
  runtime by `--build-vectors`.
- `emit_manifest.json` — the reproducible source list for `mch_ir.json`. Lists
  every spec file (relative to the mch package dir), in registration order.

## Regenerating mch_ir.json from spec sources (reproducible)

`mch_ir.json` is emitted from the committed `.mch` specs via the manifest — there
is no session-scratch emit dir. From `indexer/` with the venv python:

    python -m indexer.events.mch.ir_emit \
      --manifest ../ton-index-worker/ton-mch-engine/ir/emit_manifest.json \
      -o ../ton-index-worker/ton-mch-engine/ir/mch_ir.json

The manifest lists `specs_decl/jettons.mch` first, then the `specs/*.mch` files
in file-name order (minus the two excluded legacy jetton specs, see below);
`frontend.source_files` echoes the manifest paths. Matcher
registration order follows the list; equal-`priority` matchers tie-break on it.
The declarative jetton family carries in-spec `priority 90` (< default 100) so it
registers ahead of the priority-100 specs — this replaced the former
`a_jettons_decl.mch` filename-sort hack (plan §10 T20). Byte-reproduction is
guarded by `tests/mch/test_ir_emit.py::test_manifest_reproduces_committed_artifact`.

## Codegen is a build step — never regenerate by hand

Every generated source is produced by a CMake custom command into the build
tree (`<binary-dir>/ton-mch-engine/generated/`) and is NOT in git:

| input (git)           | generator                              | output (build tree)           | table symbols                                             |
|-----------------------|----------------------------------------|-------------------------------|-----------------------------------------------------------|
| ir/expr_vectors.json  | mch-codegen --vectors                  | vectors_generated.cpp         | `gen_vectors()`                                           |
| ir/mch_ir.json†       | mch-codegen --matchers --suffix ir     | matchers_ir_generated.cpp     | `gen_matchers_ir()`, `gen_matchers_ir_source_sha()`       |
| ir/mch_ir.json†       | mch-codegen --wheres --suffix ir       | wheres_ir_generated.cpp       | `gen_wheres_ir()`, `gen_wheres_ir_source_sha()`           |
| ir/mch_ir.json†       | mch-codegen --builds --suffix ir       | builds_ir_generated.cpp       | `gen_builds_ir()`, `gen_builds_ir_source_sha()`           |
| ir/build_vectors.json | mch-codegen --builds --suffix vectors  | builds_vectors_generated.cpp  | `gen_builds_vectors()`, `gen_builds_vectors_source_sha()` |
| tlb/mch-msgs.tlb      | tlbc                                   | mch-msgs-tlb.{cpp,h}          | `mchmsg::gen::*`                                          |

† the three CLASSIFY tables come from `MCH_CLASSIFY_ARTIFACT`, which is
`ir/mch_ir.json` unless `-DMCH_TESTBED_IR` overrides it (see below).

Editing an input JSON (or the .tlb) and rebuilding `ton-mch-engine` is the
whole workflow — CMake reruns the affected generator automatically. Each table
carries its own `--suffix` symbols, so tables from several documents link into
one binary. There is no run-time table SELECTION any more: the classify tables
are chosen at configure time, and `prepare_classify` only cross-checks that the
matcher / build / where tables agree on their source sha, which catches a
half-finished regeneration.

## Running against a different artifact

Every mode runs the tables COMPILED INTO the binary, so "running another
artifact" means building against it. `-I <path>` still parses on the CLI (the
gate scripts pass it) but is ignored with a warning. Two ways:

- **Contract swap**: overwrite `ir/mch_ir.json` and rebuild (seconds — only
  the affected tables regenerate). Use when the new artifact IS the new
  contract.
- **Testbed artifact** (`-DMCH_TESTBED_IR=<path/to/artifact.json>` cache var):
  the classify tables (matchers + builds + wheres) are generated from that
  document instead, under the SAME symbols — swapping the input is all the knob
  does. The contract file stays untouched; intended for phase work that lives
  off experimental artifacts. Note the `mch_ir_equivalence` ctest then runs
  against the testbed document too (CMake passes it the same
  `MCH_CLASSIFY_ARTIFACT`), so the gate keeps working.

## Concurrency hazard

Never share one binary dir across concurrently working agents/sessions: a
parallel edit of an input JSON mid-build can be captured by one generator run
and not another (and Syncthing can revert a file with its mtime preserved, so
ninja may not even rerun the generator). `prepare_classify`'s cross-table sha
check catches the resulting mismatch at startup, but the fix is isolation: one
binary dir per agent.

## Syncing from the frontend

Source of truth lives in the indexer frontend:

- `mch_ir.json`        <- `python -m indexer.events.mch.ir_emit <specs> -o ...`
- `expr_vectors.json`  <- indexer/tests/mch_ir/vectors/expr_vectors.json — a
  MANUAL copy; the two files are kept in sync by hand and there is
  deliberately no build-time gate that checks they still match.
- `build_vectors.json` <- no frontend source. It's GENERATED by an offline,
  uncommitted script that runs the real Python `BuildProgram.run` (see
  `src/BuildVectorRunner.h`) and the output is committed directly here — it
  is never synced from a path in the indexer tree.

Copying the refreshed `mch_ir.json` / `expr_vectors.json` here is still a
manual step; everything after the copy (regeneration, table pairing) is
handled by the build.
