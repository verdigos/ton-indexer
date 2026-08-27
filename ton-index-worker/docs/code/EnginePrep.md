---
module: EnginePrep
source: ton-index-worker/ton-mch-engine/src/EnginePrep.cpp
source: ton-index-worker/ton-mch-engine/src/EnginePrep.h
source-commit: 06d4c50
---

# EnginePrep

EnginePrep is the embedding entry point for the classify engine: it prepares the trace-independent state once, up front, and hands it to every classifier spawn as an immutable shared object. There is nothing to load at runtime — the matcher table is compiled into the binary via GenMatchers.h — so the whole startup cost is the advisory parser-registry check plus prepare_classify over that table. make_engine_prep wraps the result in a shared_ptr to const and returns errors rather than aborting, leaving the fatal-or-not decision to the caller; it performs no I/O and owns no threads. The emulator's EmuClassifierActor and ton-trace-emulator are the live consumers, with ton-mch-index using it for batch classification.

## class mch::MchEnginePrep

- decl: `ton-index-worker/ton-mch-engine/src/EnginePrep.h:25`

Plain struct bundling a pointer to the compiled matcher table with the classify setup derived from it.

matchers points at the process-lifetime generated table, and setup holds the skip table, the generated build-fn map and the referenced lookup kinds, with setup.included indexing into that table. Instances are created only by make_engine_prep and shared as const pointers across classifier spawns, so the contents are effectively read-only after construction; there is no destructor logic or ownership of the table itself.

## make_engine_prep

`td::Result<std::shared_ptr<const MchEnginePrep>> make_engine_prep()`

- decl: `ton-index-worker/ton-mch-engine/src/EnginePrep.cpp:8`
- callers: [main (ton-index-worker/ton-mch-index/src/main.cpp:27)](main.md#main); [main (ton-index-worker/ton-trace-emulator/src/main.cpp:195)](main.md#main)
- calls: `mch::gen_matchers_ir`, `mch::prepare_classify`, `mch::validate_registries`

Builds the shared immutable engine prep: checks the parser registries and prepares classify setup over the compiled-in table.

Runs validate_registries first so a name claimed by more than one parser source stops startup instead of resolving arbitrarily, then fills matchers from gen_matchers_ir and builds the setup via prepare_classify. A setup reporting a missing table or function is returned as an error; both signal build bugs, and the caller decides whether to treat them as fatal. No I/O is performed, and the result is a shared_ptr to const so every consumer shares one copy of the state.
