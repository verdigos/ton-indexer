# ir/ — version-controlled MCH policy and conformance inputs

The production matcher IR is generated into the CMake build tree from the
tracked manifest, `.mch` specs, and Python frontend. This directory keeps the
policy input and the separately maintained conformance vectors.

- `expr_vectors.json` — 127 expression conformance vectors. Input for
  `--vectors` (functions run by `--vectors-gen`); loaded at runtime by
  `--vectors` / `--vectors-gen`.
- `build_vectors.json` — 38 build-program conformance cases. Input for
  `--builds --suffix vectors` (functions run by `--build-vectors`); loaded at
  runtime by `--build-vectors`.
- `emit_manifest.json` — the reproducible source list for `mch_ir.json`. Lists
  every production spec file (relative to the mch package dir), in registration
  order. The two legacy builder-tier jetton specs remain excluded as documented
  by its `excluded_note`.

## Build-time mch_ir.json generation

On a normal build, CMake runs the emitter twice and writes:

    <binary-dir>/ton-index-worker/ton-mch-engine/generated/mch_ir.json
    <binary-dir>/ton-index-worker/ton-mch-engine/generated/mch_ir.second.json

The manifest lists `specs_decl/jettons.mch` first, then the `specs/*.mch` files
in file-name order (minus the two excluded legacy jetton specs, see below);
`frontend.source_files` echoes the manifest paths. Matcher
registration order follows the list; equal-`priority` matchers tie-break on it.
The declarative jetton family carries in-spec `priority 90` (< default 100) so it
registers ahead of the priority-100 specs — this replaced the former
`a_jettons_decl.mch` filename-sort hack (plan §10 T20). Byte-reproduction is
guarded by `mch_ir_equivalence`, which byte-compares the two build-tree outputs,
and by `tests/mch/test_ir_emit.py::test_manifest_emission_is_deterministic`.

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

Editing a `.mch` spec, frontend source, manifest, conformance JSON, or the `.tlb`
and rebuilding is the whole workflow — CMake reruns the affected generators
automatically. Each table carries its own `--suffix` symbols, so tables from
several documents link into one binary. There is no run-time table SELECTION any
more: the classify tables are chosen at configure time, and `prepare_classify`
only cross-checks that the matcher / build / where tables agree on their source
sha, which catches a half-finished regeneration.

## Running against a different artifact

Every mode runs the tables COMPILED INTO the binary, so "running another
artifact" means building against it. `-I <path>` still parses on the CLI (the
gate scripts pass it) but is ignored with a warning. Use the **testbed
artifact** option (`-DMCH_TESTBED_IR=<path/to/artifact.json>` cache var):

The classify tables (matchers + builds + wheres) are generated from that
document instead, under the SAME symbols — swapping the input is all the knob
does. The normal manifest still emits both build-tree IR copies, and
`mch_ir_equivalence` continues to compare those two deterministic emissions.

## Concurrency hazard

Never share one binary dir across concurrently working agents/sessions: a
parallel edit of an input spec or manifest mid-build can be captured by one
generator run and not another (and Syncthing can revert a file with its mtime
preserved, so ninja may not even rerun the generator). `prepare_classify`'s
cross-table sha check catches the resulting mismatch at startup, but the fix is
isolation: one binary dir per agent.

## Syncing from the frontend

Source of truth for the production matcher IR lives in the indexer frontend and
is consumed directly by CMake:

- `generated/mch_ir.json` <- `emit_manifest.json` + `.mch` specs + frontend
- `expr_vectors.json`  <- indexer/tests/mch_ir/vectors/expr_vectors.json — a
  MANUAL copy; the two files are kept in sync by hand and there is
  deliberately no build-time gate that checks they still match.
- `build_vectors.json` <- no frontend source. It's GENERATED by an offline,
  uncommitted script that runs the real Python `BuildProgram.run` (see
  `src/BuildVectorRunner.h`) and the output is committed directly here — it
  is never synced from a path in the indexer tree.

Copying a refreshed `expr_vectors.json` here is still a manual step. The
production `mch_ir.json`, generated table pairing, and determinism copy are all
handled by the build.
