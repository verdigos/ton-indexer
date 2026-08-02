# Python A/B twins for ton-mch-engine

Byte-exact Python counterparts of the engine's dump modes, used by the gate set in
`indexer/docs/mch-cpp-plan.md` §1. They import the REAL Python classifier
(`indexer.events.mch_ir` engine, `block_tree_serializer`, post-process passes), so they run
from `S:\Dev\ton-indexer-clear\indexer` with the venv python
(`S:\Dev\H-dump\tonscan-indexator\venv0\Scripts\python.exe`).

| Twin | Engine mode | Invocation (from indexer/) |
|---|---|---|
| py_msg_dump.py | `--parse-dump -T <dir>` | `python twins/py_msg_dump.py tests/traces` * |
| py_match_dump.py | `--match -I <artifact> -T <dir>` | `python <this>/py_match_dump.py <artifact> tests/traces` |
| py_classify.py | `--classify -I <artifact> -T <dir>` | `python <this>/py_classify.py <artifact> tests/traces` |
| py_actions.py | `--actions -I <artifact> -T <dir>` | `python <this>/py_actions.py <artifact> tests/traces` |

(* py_msg_dump takes just the traces dir; the others take the artifact path first.
`<artifact>` is normally
`<binary-dir>/ton-index-worker/ton-mch-engine/generated/mch_ir.json`.)

Before running any twin, build `mch_generate_surface` and set
`MCH_CPP_SURFACE_JSON` to
`<binary-dir>/ton-index-worker/ton-mch-engine/generated/cpp_surface.json`.

Gate = `Compare-Object`/diff of engine stdout vs twin stdout, expected 0 lines.

`build_mch.bat` = the ONLY sanctioned local build entrypoint (vcvars64 + CLion cmake, see
plan §0.9). Cap parallelism when the user works in parallel:
`$env:CMAKE_BUILD_PARALLEL_LEVEL=4` (or copy the bat and add `-j 4`).

## C++ registry surface (cpp_surface.json / cpp_surface.py)

The twins gate matcher runnability against the C++ host/parse registries
(message parsers, host preds/fns/shapers, lookup kinds, BOC field names). Those
sets used to be hand-mirrored inline in each twin (CPP_PARSERS etc.) and drifted
independently. They now come from one build-tree file:

- `generated/cpp_surface.json` — emitted from the LIVE C++ registries
  (stable-sorted JSON). Regenerate after adding a parser / host binding / lookup
  kind / BOC field:

      cmake --build <binary-dir> --target mch_generate_surface

- `cpp_surface.py` — loads that JSON and exposes `CPP_PARSERS`,
  `CPP_LOOKUP_KINDS`, `CPP_SHAPERS`, `CPP_PREDS`, `CPP_FNS`, `BOC_FIELDS`. All
  four twins import from it (each imports only what it uses). It reads the path
  from `MCH_CPP_SURFACE_JSON`.

`BUILTINS` stays inline in py_classify/py_actions — it is the fixed expr-builtin
set, not a host registry. `lookup_kinds` includes `dedust_pool` (fetched by the
dedust_swap_legs host fn, never a `lookup` node) — an honest registry superset
that moves no skip decision. The generated JSON is guarded by
`indexer/tests/mch/test_cpp_surface.py` and, for output-affecting drift, by the
classify/parse A/B gates themselves.

History: these lived in session scratchpads (plan §0.4 originally said "NOT committed");
moved into the repo 2026-07-11 for fresh-context durability — they encode gate semantics
(field-keyed cellhash renders, canonical sorting, skip tables) that are expensive to
reconstruct.
