"""Single source of truth for the C++ registry surface used by the A/B twins.

Loads ``cpp_surface.json`` (emitted by ``ton-mch-engine --surface`` from the live
C++ registries) and exposes the sets the twins gate matcher runnability on. This
replaces the CPP_PARSERS/CPP_PREDS/CPP_FNS/CPP_SHAPERS/CPP_LOOKUP_KINDS/BOC_FIELDS
literals that were hand-mirrored in each twin (A3(ii)) — one file, no per-twin drift.

Regenerate after adding a parser / host binding / lookup kind / BOC field:

    ton-mch-engine --surface > ton-index-worker/ton-mch-engine/twins/cpp_surface.json

Output-affecting drift between this file and the C++ registries is caught by the
classify/parse A/B gates (a stale surface makes the twin skip a matcher the engine
runs, or vice versa → diff != 0). tests/mch/test_ir_emit.py additionally checks
the committed file against a live ``--surface`` when the engine exe is present.

Note: ``lookup_kinds`` includes ``dedust_pool`` (fetched by the dedust_swap_legs
host fn, never a `lookup` build-program node) — an honest registry superset that
never appears in a matcher's ref_lookups, so it moves no skip decision.
"""
from __future__ import annotations

import json
from pathlib import Path

_data = json.loads((Path(__file__).with_name("cpp_surface.json")).read_text(encoding="utf-8"))

CPP_PARSERS = set(_data["parsers"])
CPP_LOOKUP_KINDS = set(_data["lookup_kinds"])
CPP_SHAPERS = set(_data["shapers"])
CPP_PREDS = set(_data["preds"])
CPP_FNS = set(_data["fns"])
BOC_FIELDS = set(_data["boc_fields"])
