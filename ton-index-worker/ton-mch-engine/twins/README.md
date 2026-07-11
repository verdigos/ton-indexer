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
`<artifact>` is normally `ton-index-worker/ton-mch-engine/ir/mch_ir.json`.)

Gate = `Compare-Object`/diff of engine stdout vs twin stdout, expected 0 lines.

`build_mch.bat` = the ONLY sanctioned local build entrypoint (vcvars64 + CLion cmake, see
plan §0.9). Cap parallelism when the user works in parallel:
`$env:CMAKE_BUILD_PARALLEL_LEVEL=4` (or copy the bat and add `-j 4`).

History: these lived in session scratchpads (plan §0.4 originally said "NOT committed");
moved into the repo 2026-07-11 for fresh-context durability — they encode gate semantics
(field-keyed cellhash renders, canonical sorting, skip tables, CPP_* surface lists) that are
expensive to reconstruct. The CPP_* surface lists inside py_classify/py_match_dump must be
kept in sync with the C++ registries when host bindings/parsers are added (grep for
CPP_PARSERS / CPP_PREDS / CPP_FNS / CPP_SHAPERS / LOOKUP_KINDS / BOC_FIELDS).
