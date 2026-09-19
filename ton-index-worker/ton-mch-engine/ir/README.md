# ir/ — version-controlled MCH policy and conformance inputs

The production matcher IR is generated into the CMake build tree from the
built-in ordered source list in `ir_emit.py`, the `.mch` specs, and the compiler
frontend. This directory keeps the separately maintained conformance vectors.

- `expr_vectors.json` is the canonical copy of 229 expression conformance vectors.
  `generate_expr_vectors.py` emits their IR through the compiler frontend.
  CMake compiles them into `ton-mch-engine`; ctest `mch_expr_vectors` runs them
  with `--expr-vectors-test`.
- `build_vectors.json` contains 81 build-program conformance cases. CMake compiles
  them into `ton-mch-engine`; ctest `mch_build_vectors` runs them with
  `--build-vectors-test`.

## Build-time mch_ir.json generation

On a normal build, CMake runs the emitter twice and writes:

    <binary-dir>/ton-index-worker/ton-mch-engine/generated/mch_ir.json
    <binary-dir>/ton-index-worker/ton-mch-engine/generated/mch_ir.second.json

`ir_emit.py`'s built-in source list places `specs/jettons_decl.mch` at its
existing registration position among the other `specs/*.mch` files (minus the
two excluded legacy jetton specs, see below); `frontend.source_files` echoes
those paths. Matcher registration order follows the list; equal-`priority`
matchers tie-break on it.

The declarative jetton family sets `priority 90`, so it registers ahead of the
priority-100 specs. `mch_ir_equivalence` byte-compares the two build-tree
outputs, and `tests/mch/test_ir_emit.py::test_manifest_emission_is_deterministic`
checks the manifest emission order.

## Codegen is a build step — never regenerate by hand

Every generated source is produced by a CMake custom command into the build
tree (`<binary-dir>/ton-mch-engine/generated/`) and is NOT in git:

| input                  | generator                              | output (build tree)           | table symbols                                             |
|------------------------|----------------------------------------|-------------------------------|-----------------------------------------------------------|
| ir/expr_vectors.json   | mch-codegen --vectors                  | vectors_generated.cpp         | `gen_vectors()`                                           |
| generated/mch_ir.json† | mch-codegen --matchers --suffix ir     | matchers_ir_generated.cpp     | `gen_matchers_ir()`, `gen_matchers_ir_source_sha()`       |
| generated/mch_ir.json† | mch-codegen --wheres --suffix ir       | wheres_ir_generated.cpp       | `gen_wheres_ir()`, `gen_wheres_ir_source_sha()`           |
| generated/mch_ir.json† | mch-codegen --builds --suffix ir       | builds_ir_generated.cpp       | `gen_builds_ir()`, `gen_builds_ir_source_sha()`           |
| ir/build_vectors.json  | mch-codegen --builds --suffix vectors  | builds_vectors_generated.cpp  | `gen_builds_vectors()`, `gen_builds_vectors_source_sha()` |
| tlb/mch-msgs.tlb       | tlbc                                   | mch-msgs-tlb.{cpp,h}          | `mchmsg::gen::*`                                          |

† the three CLASSIFY tables come from `MCH_CLASSIFY_ARTIFACT`, which is
the generated `mch_ir.json` unless `-DMCH_TESTBED_IR` overrides it (see below).

Editing a `.mch` spec, frontend source, or the `.tlb` and rebuilding is the
whole workflow — CMake reruns the affected generators automatically. Each IR
table carries its own `--suffix` symbols. There is no run-time table SELECTION any
more: the classify tables are chosen at configure time, and `prepare_classify`
only cross-checks that the matcher / build / where tables agree on their source
sha, which catches a half-finished regeneration.

## Running against a different artifact

Every mode runs the tables COMPILED INTO the binary, so "running another
artifact" means building against it. The engine CLIs do not accept `-I`; use
the **testbed artifact** option
(`-DMCH_TESTBED_IR=<path/to/artifact.json>` cache variable):

The classify tables (matchers + builds + wheres) are generated from that
document instead, under the SAME symbols — swapping the input is all the knob
does. The built-in source list still emits both build-tree IR copies, and
`mch_ir_equivalence` continues to compare those two deterministic emissions.

## Concurrency hazard

Never share one binary dir across concurrently working agents/sessions: a
parallel edit of an input spec or the built-in source list mid-build can be
captured by one generator run and not another (and Syncthing can revert a file
with its mtime preserved, so ninja may not even rerun the generator).
`prepare_classify`'s
cross-table sha check catches the resulting mismatch at startup, but the fix is
isolation: one binary dir per agent.

## Syncing from the frontend

Source of truth for the production matcher IR lives in the indexer frontend and
is consumed directly by CMake:

- `generated/mch_ir.json` <- `ir_emit.py` built-in source list + `.mch` specs
  + frontend
- `expr_vectors.json` <- `generate_expr_vectors.py` in this directory, run from
  the repository root as
  `PYTHONPATH=indexer python ton-index-worker/ton-mch-engine/ir/generate_expr_vectors.py <out.json>`.
  Every vector's `expr` comes from the compiler frontend; `env` and `expect`
  are authored in the generator.
- `build_vectors.json` is the committed build-program corpus. It has no
  frontend source or separate copy in the indexer tree.

The `mch_expr_vectors` and `mch_build_vectors` ctests run the committed corpora
through the C++ engine. The production `mch_ir.json`, generated table pairing,
and determinism copy are handled by the build.
