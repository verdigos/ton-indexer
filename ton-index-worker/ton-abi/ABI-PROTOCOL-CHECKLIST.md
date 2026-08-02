# Adding a protocol through the ABI path

This checklist covers every touch point for putting an on-chain message family
through a `.tolk` declaration, committed ABI JSON, generated parser pair, mch
registry row, `.mch` spec, and gated Action row. Follow it in order.

`W11-SHAPE-SPEC.md` defines the emitter contract. `MODULE-NOTES.md` defines the
canonical value shapes and wire-format invariants used by the bridge.

## 1. Declare the messages (ton-abi)

1. `cp abi/_TEMPLATE.tolk abi/<contract>.tolk` and fill the struct slot. Declare
   only the messages the spec parses. Copy opcodes and field names/types
   VERBATIM from the authoritative contract source. Everything below the slot is
   compiler appeasement (tolk needs a contract + entrypoint to emit an ABI);
   name your structs in the union arm and leave the rest alone.
2. Add the file stem to `PROD_DECLS` in `tools/gen_prod_abi.mjs`.
3. `node tools/gen_prod_abi.mjs` → writes `abi/<contract>.abi.json`. Commit BOTH
   the `.tolk` and the `.abi.json`. (`--check` proves the committed JSON still
   matches a fresh compile; `_TEMPLATE.tolk` is skipped because PROD_DECLS is an
   explicit list, not a glob.)
4. Regenerate the parser pair into `generated/`:
   `ton-abi-gen abi/<contract>.abi.json --out-dir generated` → commit
   `generated/<contract>_gen.{h,cpp}`.
   **ton-abi's CMakeLists needs NO edit**: `generated/*_gen.cpp` and
   `abi/*.abi.json` are both `file(GLOB … CONFIGURE_DEPENDS)`, so the new pair is
   compiled into the test exe and covered by the `ton_abi_gen_check` byte-repro
   gate automatically.

## 2. Register the parsers (mch bridge)

5. `ton-mch-engine/src/AbiBridge.cpp`: `#include "<contract>_gen.h"` and add one
   row per message to `abi_message_parsers()`:
   `{"<DeclName>", &abi_parse_body<ton_abi::gen::<contract>::<DeclName>>}`.
   Keys are **BARE decl names** (the `.mch` grammar's `parse CAP as Name` reads a
   bare IDENT; `::` does not lex). A name that collides with an existing row is
   caught by `validate_registries()` at startup and fails closed in
   `parse_message_body`.
6. `ton-mch-engine/CMakeLists.txt`: append the pair to `ABI_PROD_PAIRS` **only
   if this is a NEW `.tolk` file**. A new decl inside an existing contract's
   `.tolk` needs no CMake edit at all.
7. `src/AbiBridgeTest.cpp`: the `test_abi_rows_dispatch()` loop already asserts
   every row is registered under its own key and routes through
   `parse_message_body`, so a new row is covered without a new test. Add explicit
   round-trip asserts for anything shape-interesting (a new adapter kind, an
   address whose exact render matters).

## 3. Python twin parsers

8. `indexer/indexer/events/mch/builders/<protocol>.py`: hand-written pytoniq
   parser classes mirroring the ADAPTER's shape, plus a `register_*` and a
   `*_registries()` factory. Load-bearing rules:
   - field names are the **tolk camelCase** ABI names, not the snake_case of
     legacy `blocks/messages/*.py`. The spec accessor, adapter Obj key, and
     the Python attribute must be the same string;
   - value kinds must match the adapter and the table in `MODULE-NOTES.md`; notably
     addresses are exposed already-typed as `AccountId`;
   - **op-guard rule**: the ABI parsers CHECK their own opcode prefix, the Python
     twins do NOT, so every ABI-path `parse` capture must sit behind an opcode
     guard in the spec, or the two sides disagree on a non-matching body.
9. `builders/__init__.py`: import and `__all__`-export BOTH `<x>_registries` and
   `register_<x>`, like the sibling modules.
10. `ton-mch-engine/twins/twin_registries.py`: import the factory and append it to
    `_FACTORIES`. This one edit updates all three twins.

## 4. Spec + artifact

11. `indexer/indexer/events/mch/specs/<name>.mch`: the matcher. `produces <snake>`
    is the btype and therefore `action.type`.
12. `ton-mch-engine/ir/emit_manifest.json`: add the spec path. CMake emits
    `generated/mch_ir.json` from the manifest and regenerates the paired build
    tables on the next build; a stale pairing is a hard error, not a silent skip.

## 5. Action serialization (usually the bulk)

13. Python: `_fill_<name>(block, action)` + a `case` in `block_to_action`
    (`blocks/utils/block_tree_serializer.py`). Reuse an existing composite type
    where the fields fit rather than adding DDL.
14. C++: `fill_<name>(d, a)` + a branch in `fill_action`
    (`ton-mch-engine/src/ActionBuild.cpp`).
15. If the composite column is new to the mch render surface, add it to BOTH
    render column sets: `Classify.cpp render_action` and `twins/py_actions.py`
    `ACTION_COLUMNS`.
16. `twins/py_actions.py`: add the btype to `PORTED_BTYPES` (and update the count
    in the comment above it), otherwise the action is silently not gated.

## 6. Fixtures + regen

17. Put a real trace fixture in `indexer/tests/traces/*.lz4` (see the
    `hotcold_kvrocks_trace_fetch` notes for fetching one). Without a fixture the
    leg has only unit coverage, which should be stated explicitly in coverage
    notes.
18. Regenerate `twins/cpp_surface.json`: `ton-mch-engine --surface > …/twins/cpp_surface.json`.
    New parser rows are expected to change this file.

## 7. Gates (all must be green; STOP and report rather than tweak toward green)

- build: `twins/build_mch.bat` via the PowerShell tool, never Bash. If the CMake
  file changed, configure FIRST in a plain environment (never under vcvars).
  Never build the `all` target; use named targets only.
- `ctest -R mch` (incl. `mch_abi_bridge`); ton-abi `ctest` (`ton_abi_smoke` +
  `ton_abi_gen_check`).
- twin diffs from `indexer/` with the venv python: parse-dump / match / classify /
  actions all diff-0; `--classify-schema` / `--actions-schema` and
  `--classify-prod` / `--actions-prod` vs their base modes diff-0. Action counts
  move by exactly the rows the new protocol adds. Account for every one.
- `python -m pytest tests/`: no new failures.
