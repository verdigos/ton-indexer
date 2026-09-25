# ir/: expression conformance vectors and IR build notes

This directory contains `generate_expr_vectors.py`. The script passes each
expression through the compiler frontend and emits the expression conformance
vectors. CMake writes them to:

    <binary-dir>/ton-index-worker/ton-mch-engine/generated/expr_vectors.json

`mch-codegen --vectors` converts that JSON to
`expr_vectors_test_generated.cpp`. The `mch_expr_vectors` test runs the
generated corpus through the C++ engine.

## Build-time IR generation

`indexer.events.mch.ir_emit` compiles the production `.mch` specs and emits
matchers in `indexer/indexer/events/mch/specs/manifest.yaml` order. CMake runs
the emitter twice and writes:

    <binary-dir>/ton-index-worker/ton-mch-engine/generated/mch_ir.json
    <binary-dir>/ton-index-worker/ton-mch-engine/generated/mch_ir.second.json

The `mch_ir_equivalence` test byte-compares these files. The frontend test
`test_manifest_emission_is_deterministic` also checks repeated manifest
emission.

## Generated files

CMake writes every generated file below to
`<binary-dir>/ton-index-worker/ton-mch-engine/generated/`. These files are not
tracked in git.

| input | generator | output |
|---|---|---|
| production `.mch` specs and `specs/manifest.yaml` | `python -m indexer.events.mch.ir_emit` | `mch_ir.json`, `mch_ir.second.json` |
| `ir/generate_expr_vectors.py` | Python | `expr_vectors.json` |
| `expr_vectors.json` | `mch-codegen --vectors` | `expr_vectors_test_generated.cpp` |
| `MCH_CLASSIFY_ARTIFACT` | `mch-codegen --matchers --suffix ir` | `matchers_ir_generated.cpp` |
| `MCH_CLASSIFY_ARTIFACT` | `mch-codegen --wheres --suffix ir` | `wheres_ir_generated.cpp` |
| `MCH_CLASSIFY_ARTIFACT` | `mch-codegen --builds --suffix ir` | `builds_ir_generated.cpp` |
| `MCH_CLASSIFY_ARTIFACT` | `mch-codegen --btypes` | `btypes_gen.h` |
| `payload-keys.yaml` | `python -m indexer.events.mch.payload_keys cpp` | `payload_keys_generated.h` |

Editing a `.mch` spec, the manifest, a frontend source, the vector generator,
or `payload-keys.yaml` and rebuilding reruns the affected generator.

## Selecting the classification artifact

The classify tables are chosen at configure time. By default,
`MCH_CLASSIFY_ARTIFACT` is the generated `mch_ir.json`. Configure with
`-DMCH_TESTBED_IR=<path/to/artifact.json>` to generate the matcher, build,
where, and btype tables from another artifact.

All four outputs use the same configured artifact. At startup,
`prepare_classify` checks that the matcher, build, and where tables contain the
same source SHA.

## Concurrent builds

Do not share one binary directory across concurrent builds. An input edit
between generator runs can leave mismatched outputs. Use one binary directory
per build.
