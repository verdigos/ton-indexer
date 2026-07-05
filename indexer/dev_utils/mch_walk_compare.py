"""RETIRED in T29 stage 2 — the compiled path this tool compared against is gone.

This tool used to compare the FINAL MATCHER TREE WALK of an mch-*compiled*
matcher against the legacy hand-written matcher it was ported from — a
traversal-level parity check, not just a result check. It worked by importing
`tools.matcher_trace` to install dormant wrappers on every `BlockMatcher` method
(`test_self` / `try_build` / `build_block` / `process_child_matcher` /
`process_parent_matcher`), running BOTH the legacy matcher and the compiled
`_MCH_<name>` matcher inside `mt.record()`, then folding away the compiled-path
walk artifacts (the generated anchor's `try_build` override calling
`base.try_build`; the `_extract_captures` probe storm under `build_block`) and
diffing the two normalised event streams.

Every one of those load-bearing pieces was a compiled-path artifact:
  * the compiled matcher was a runnable `BlockMatcher` instance
    (`CompiledMatcher.matcher`) — T29 stage 2 replaced it with a plain
    `CompiledMatcher.root` IR node graph (`compiler.LNode`) that is NOT runnable
    and carries no instrumentable methods;
  * the folded `_MCH_` anchor `try_build` wrapper and the `_extract_captures`
    capture-binding walk no longer exist.

The v2 IR engine (`indexer/events/mch_ir/`) has a different runtime shape
(`IrMatcher`), so a walk-level comparison against it would need its own
instrumentation harness — out of proportion for this dev tool. The surviving
parity guarantees now live in:
  * `tests/mch/integration/*` — per-protocol legacy vs IR field/topology parity,
  * `tests/mch/integration/test_parity_sweep.py` — the corpus-wide legacy vs IR
    sweep,
  * `tests/mch/integration/snapshots/` — deterministic IR conformance snapshots.

If a traversal-level legacy-vs-IR comparison is ever wanted, build a fresh
harness around the IR engine's own call structure rather than resurrecting this
`BlockMatcher`-instrumentation approach.
"""
from __future__ import annotations

import sys


def main() -> None:
    sys.stderr.write(__doc__)
    raise SystemExit(
        "mch_walk_compare.py is retired: the compiled matcher path it compared "
        "against was deleted in T29 stage 2. See this file's module docstring."
    )


if __name__ == "__main__":
    main()
