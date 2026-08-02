"""Single source of truth for the C++ registry surface used by the A/B twins.

Loads the build-tree ``cpp_surface.json`` emitted from the live C++ registries
and exposes the sets the twins gate matcher runnability on. This
replaces the CPP_PARSERS/CPP_PREDS/CPP_FNS/CPP_SHAPERS/CPP_LOOKUP_KINDS/BOC_FIELDS
literals that were hand-mirrored in each twin (A3(ii)) — one file, no per-twin drift.

Build it after adding a parser / host binding / lookup kind / BOC field:

    cmake --build <binary-dir> --target mch_generate_surface

Set ``MCH_CPP_SURFACE_JSON`` to the generated file before running a twin.

Output-affecting drift between this file and the C++ registries used to be left
to the classify/parse A/B gates to catch (a stale surface makes the twin skip a
matcher the engine runs → diff != 0). That is TOO LATE and reads wrong: the
diff shows the skipped matcher's rows replaced by raw call_contract leaves, i.e.
it looks like a matcher regression, and because this file is regenerated from
whatever binary an agent happens to have built, re-running after someone else's
regen gives a DIFFERENT answer — the twin reads as nondeterministic when it is
perfectly deterministic per surface state. `assert_no_skips` below turns that
into an immediate, self-naming error instead. tests/mch/test_cpp_surface.py
additionally checks the generated file against a live ``--surface`` when the
engine executable is present.

Note: ``lookup_kinds`` includes ``dedust_pool`` (fetched by the dedust_swap_legs
host fn, never a `lookup` build-program node) — an honest registry superset that
never appears in a matcher's ref_lookups, so it moves no skip decision.
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

_surface_path = os.environ.get("MCH_CPP_SURFACE_JSON")
if not _surface_path:
    raise RuntimeError(
        "MCH_CPP_SURFACE_JSON must point to the mch_generate_surface build output "
        "(<binary-dir>/ton-mch-engine/generated/cpp_surface.json)"
    )
_data = json.loads(Path(_surface_path).read_text(encoding="utf-8"))

CPP_PARSERS = set(_data["parsers"])
CPP_LOOKUP_KINDS = set(_data["lookup_kinds"])
CPP_SHAPERS = set(_data["shapers"])
CPP_PREDS = set(_data["preds"])
CPP_FNS = set(_data["fns"])
BOC_FIELDS = set(_data["boc_fields"])

# Escape hatch for a TESTBED artifact whose matchers legitimately outrun the
# generated C++ surface (-DMCH_TESTBED_IR work). Never set it for a gate run
# against the default generated/mch_ir.json.
ALLOW_SKIPS = os.environ.get("MCH_TWIN_ALLOW_SKIPS") == "1"


def assert_no_skips(skips) -> None:
    """Abort when the generated surface cannot serve the artifact being run.

    `skips` is the [(matcher_name, reason)] list the caller computed with its
    own `skip_reason`. Every matcher in generated/mch_ir.json is runnable by the
    C++ engine — that is what makes it the PRODUCTION artifact — so a skip there
    is never a property of the run: it means the generated `cpp_surface.json`
    and `mch_ir.json` disagree. Continuing would emit a smaller, entirely
    plausible row set and quietly move the golden diff.
    """
    if not skips or ALLOW_SKIPS:
        return
    lines = "\n".join(f"  {name}: {reason}" for name, reason in skips)
    sys.exit(
        "twin ABORT: the generated cpp_surface.json cannot serve this artifact, so "
        f"{len(skips)} matcher(s) would be dropped:\n{lines}\n"
        "cpp_surface.json is a MIRROR of the C++ registries — a missing "
        "fns:/types:/lookups:/pred:/shaper: name means it was regenerated from a "
        "build that predates those registrations. Rebuild the surface with:\n"
        "  cmake --build <binary-dir> --target mch_generate_surface\n"
        "(Running a testbed artifact where skips are intended? MCH_TWIN_ALLOW_SKIPS=1.)"
    )
