"""T29 — benchmark the MCH matcher runtimes.

For every spec row in the parity sweep's `_SPEC_TABLE` and every corpus fixture
that row's opcode anchors could fire on, time the *matcher pass + build* for:

    legacy    - the hand-written BlockMatcher(s) in indexer/events/blocks/*.py
    ir        - compile_file -> emit_ir -> load_artifact -> build_matchers

The compiled frontend leg was deleted in T29 stage 2 (the frontend now lowers
straight to IR node records — there is no runnable compiled matcher); the
benchmark is legacy-vs-IR only. `mch_bench_results.md` retains the historical
three-runtime table with a note marking its `compiled` column pre-stage-2.

Methodology
-----------
* Fixture selection, trace loading, tree construction and the prereq-matcher
  prelude are REUSED verbatim from tests/mch/integration/test_parity_sweep.py
  (imported directly: `_SPEC_TABLE`, `_MATCHERS`, `_TRACE_CACHE`,
  `_build_block_tree`, `compile_file`, `emit_ir`, `_mch_ir`). No plumbing is
  reinvented. `compile_file` is still called once per row to produce the IR
  emit source; its `CompiledMatcher.root` graph is not run directly.
* Matching mutates the block tree, so a FRESH tree is built for every timed run
  and never reused. Tree construction and the prereq passes are OUTSIDE the
  timed region (they are identical across the three modes, so they cancel).
* Per (row, trace, mode): 2 warmup runs then N=5 timed runs, median of the 5,
  measured with time.perf_counter_ns.
* Compile / emit / load_artifact / matcher construction all happen ONCE per row,
  outside timing (per the T29 note).

Row granularity: a spec may emit several named matchers (jettons -> 3, jvault ->
4, dedust_deposit -> 2). The timed region runs the whole set over the tree, and
the legacy leg runs the corresponding hand-written class set (LEGACY_FOR). Where
the DSL splits one legacy matcher into several (stonfi_v2_provide) the set sizes
differ; that asymmetry is inherent to the port and is noted in the report.

Standalone: `asyncio.run(main())`, NOT a pytest test. Rows that cannot run
(missing legacy mapping, no candidate fixture) are skipped with a printed
reason, never crashing the whole run.
"""
from __future__ import annotations

import asyncio
import statistics
import sys
import time
from pathlib import Path

# Import the sweep harness (does all fixture/spec/tree plumbing at module load).
_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from indexer.events import context  # noqa: E402
from tests.utils.repository import TestInterfaceRepository  # noqa: E402
import tests.mch.integration.test_parity_sweep as sweep  # noqa: E402

# --- legacy matcher map: spec_key -> hand-written BlockMatcher classes --------
from indexer.events.blocks.jettons import (  # noqa: E402
    JettonBurnBlockMatcher,
    JettonMintBlockMatcher,
    JettonTransferBlockMatcher,
)
from indexer.events.blocks.swaps import (  # noqa: E402
    CoffeeSwapBlockMatcher,
    DedustSwapBlockMatcher,
    StonfiSwapBlockMatcher,
    StonfiV2SwapBlockMatcher,
    ToncoSwapBlockMatcher,
)
from indexer.events.blocks.liquidity import (  # noqa: E402
    CoffeeCreateVaultMatcher,
    DedustDepositBlockMatcher,
    DedustDepositFirstAssetBlockMatcher,
    DedustWithdrawBlockMatcher,
    StonfiV2ProvideLiquidityMatcher,
    StonfiV2WithdrawLiquidityMatcher,
    ToncoDepositLiquidityMatcher,
)
from indexer.events.blocks.staking import (  # noqa: E402
    NominatorPoolWithdrawMatcher,
    TONStakersWithdrawMatcher,
)
from indexer.events.blocks.nft import (  # noqa: E402
    GetgemsNftPurchaseBlockMatcher,
    TelegramNftPurchaseBlockMatcher,
)
from indexer.events.blocks.dns import ChangeDnsRecordMatcher  # noqa: E402
from indexer.events.blocks.layerzero import LayerZeroReceiveMatcher  # noqa: E402
from indexer.events.blocks.evaa import EvaaWithdrawBlockMatcher  # noqa: E402
from indexer.events.blocks.ethena_staking import EthenaDepositBlockMatcher  # noqa: E402
from indexer.events.blocks.vesting import VestingSendMessageBlockMatcher  # noqa: E402
from indexer.events.blocks.tgbtc import TgBTCMintBlockMatcher  # noqa: E402
from indexer.events.blocks.jvault import (  # noqa: E402
    JVaultClaimBlockMatcher,
    JVaultStakeBlockMatcher,
    JVaultUnstakeBlockMatcher,
    JVaultUnstakeRequestBlockMatcher,
)

LEGACY_FOR: dict[str, list] = {
    "specs/jettons.mch": [JettonTransferBlockMatcher, JettonBurnBlockMatcher, JettonMintBlockMatcher],
    "specs/jetton_transfer.mch": [JettonTransferBlockMatcher],
    "specs/coffee_swap.mch": [CoffeeSwapBlockMatcher],
    "specs/dedust_swap.mch": [DedustSwapBlockMatcher],
    "specs/dedust_withdraw.mch": [DedustWithdrawBlockMatcher],
    "specs/dns.mch": [ChangeDnsRecordMatcher],
    "specs/stonfi_v2_swap.mch": [StonfiV2SwapBlockMatcher],
    "specs_decl/jettons.mch": [JettonTransferBlockMatcher, JettonBurnBlockMatcher, JettonMintBlockMatcher],
    "specs_decl/dedust_swap_hybrid.mch": [DedustSwapBlockMatcher],
    "specs/evaa.mch": [EvaaWithdrawBlockMatcher],
    "specs/layerzero.mch": [LayerZeroReceiveMatcher],
    "specs/tonstakers.mch": [TONStakersWithdrawMatcher],
    "specs/ethena.mch": [EthenaDepositBlockMatcher],
    "specs/vesting.mch": [VestingSendMessageBlockMatcher],
    "specs/dedust_deposit.mch": [DedustDepositBlockMatcher, DedustDepositFirstAssetBlockMatcher],
    "specs/tonco_liquidity.mch": [ToncoDepositLiquidityMatcher],
    "specs/coffee_liquidity.mch": [CoffeeCreateVaultMatcher],
    "specs/jvault.mch": [JVaultStakeBlockMatcher, JVaultUnstakeBlockMatcher, JVaultClaimBlockMatcher, JVaultUnstakeRequestBlockMatcher],
    "specs/telegram_nft.mch": [TelegramNftPurchaseBlockMatcher],
    "specs/tgbtc.mch": [TgBTCMintBlockMatcher],
    "specs/stonfi_v2_withdraw.mch": [StonfiV2WithdrawLiquidityMatcher],
    "specs/nominator.mch": [NominatorPoolWithdrawMatcher],
    "specs/tonco_swap.mch": [ToncoSwapBlockMatcher],
    "specs/getgems.mch": [GetgemsNftPurchaseBlockMatcher],
    "specs/stonfi_v2_provide.mch": [StonfiV2ProvideLiquidityMatcher],
    "specs/stonfi_v1_swap.mch": [StonfiSwapBlockMatcher],
}

WARMUP = 2
N = 5


async def _run_pass(matchers: list, prereq_instances: list, trace) -> int:
    """Build a fresh tree, run prereqs (untimed), then time the matcher pass.
    Returns the timed region duration in nanoseconds."""
    tree = sweep._build_block_tree(trace)
    for pre in prereq_instances:
        for b in tree.bfs_iter():
            if b.parent is None:
                await pre.try_build(b)
    t0 = time.perf_counter_ns()
    for matcher in matchers:
        for b in tree.bfs_iter():
            if b.btype == "root" or b.parent is not None:
                continue
            await matcher.try_build(b)
    return time.perf_counter_ns() - t0


async def _median_ns(matchers, prereq_instances, trace) -> int:
    for _ in range(WARMUP):
        await _run_pass(matchers, prereq_instances, trace)
    samples = [await _run_pass(matchers, prereq_instances, trace) for _ in range(N)]
    return int(statistics.median(samples))


def _row_fixtures(spec_key: str) -> list[str]:
    """Union of the sweep's opcode-anchor prefilter over the row's matcher
    entries. A btype-anchored entry (anchor_opcodes is None) => run on all."""
    entries = [e for e in sweep._MATCHERS if e.spec_key == spec_key]
    if any(e.anchor_opcodes is None for e in entries):
        return sorted(sweep._TRACE_CACHE.keys())
    anchors: set[int] = set()
    for e in entries:
        if e.anchor_opcodes:
            anchors |= set(e.anchor_opcodes)
    return sorted(
        fn for fn, (_t, _i, ops) in sweep._TRACE_CACHE.items() if anchors & ops
    )


async def main() -> None:
    if not sweep._HAVE_IR:
        print(f"IR engine not importable ({sweep._IR_IMPORT_ERR}); cannot benchmark.")
        return

    rows: list[dict] = []
    skipped: list[tuple[str, str]] = []

    # Aggregate ns accumulators (sum of per-trace medians across the corpus).
    agg = {"legacy": 0, "ir": 0}

    for spec_key, path, factory, prereqs in sweep._SPEC_TABLE:
        if spec_key not in LEGACY_FOR:
            skipped.append((spec_key, "no legacy matcher mapping"))
            continue
        fixtures = _row_fixtures(spec_key)
        if not fixtures:
            skipped.append((spec_key, "no candidate fixture (anchor absent from corpus)"))
            continue

        # Build the matcher sets ONCE, outside timing. `compiled` is the IR emit
        # source only (its node graph is not run — no compiled runtime exists).
        try:
            compiled = sweep.compile_file(path, factory())
            artifact = sweep.emit_ir(compiled, [path.name])
            art = sweep._mch_ir.load_artifact(artifact)
            ir_matchers = list(sweep._mch_ir.build_matchers(art, factory()))
            legacy_matchers = [cls() for cls in LEGACY_FOR[spec_key]]
            prereq_instances = [cls() for cls in prereqs]
        except Exception as ex:  # defensive: a broken row must not kill the run
            skipped.append((spec_key, f"build error {type(ex).__name__}: {ex}"))
            continue

        per = {"legacy": [], "ir": []}
        for fn in fixtures:
            trace, interfaces, _ops = sweep._TRACE_CACHE[fn]
            context.interface_repository.set(TestInterfaceRepository(interfaces))
            try:
                per["legacy"].append(await _median_ns(legacy_matchers, prereq_instances, trace))
                per["ir"].append(await _median_ns(ir_matchers, prereq_instances, trace))
            except Exception as ex:
                skipped.append((f"{spec_key}::{fn}", f"run error {type(ex).__name__}: {ex}"))
                # drop this fixture from all three legs to keep them aligned
                for k in per:
                    if len(per[k]) > len(per["ir"]):
                        per[k].pop()
                continue

        if not per["ir"]:
            skipped.append((spec_key, "all fixtures errored"))
            continue

        for k in agg:
            agg[k] += sum(per[k])

        leg_med = statistics.median(per["legacy"]) / 1000.0
        ir_med = statistics.median(per["ir"]) / 1000.0
        rows.append({
            "matcher": spec_key,
            "n": len(per["ir"]),
            "legacy_us": leg_med,
            "ir_us": ir_med,
            "ir_ratio": (ir_med / leg_med) if leg_med else float("nan"),
        })
        print(f"  done {spec_key:34s} n={len(per['ir']):3d}  "
              f"legacy={leg_med:8.1f}us ir={ir_med:8.1f}us  "
              f"ir/legacy={rows[-1]['ir_ratio']:.2f}")

    _emit(rows, skipped, agg)


def _emit(rows: list[dict], skipped: list, agg: dict) -> None:
    overall_ir = (agg["ir"] / agg["legacy"]) if agg["legacy"] else float("nan")

    hdr = f"{'matcher':34s} {'n':>4s} {'legacy us':>11s} {'ir us':>10s} {'ir/leg':>7s}"
    sep = "-" * len(hdr)
    lines = [hdr, sep]
    for r in sorted(rows, key=lambda x: x["ir_ratio"], reverse=True):
        lines.append(
            f"{r['matcher']:34s} {r['n']:>4d} {r['legacy_us']:>11.1f} "
            f"{r['ir_us']:>10.1f} {r['ir_ratio']:>7.2f}"
        )
    lines.append(sep)
    lines.append(
        f"{'AGGREGATE (sum-of-medians)':34s} {'':>4s} "
        f"{agg['legacy']/1000.0:>11.1f} {agg['ir']/1000.0:>10.1f} {overall_ir:>7.2f}"
    )
    table = "\n".join(lines)

    gate = "PASS" if overall_ir <= 1.5 else "FAIL"
    verdict = (
        f"Gate: IR engine within ~1.5x of legacy per-trace match time.\n"
        f"Overall IR/legacy = {overall_ir:.2f}x  ->  {gate}"
    )

    print("\n" + table + "\n\n" + verdict)
    if skipped:
        print("\nSkipped rows:")
        for k, why in skipped:
            print(f"  {k}: {why}")

    md = [
        "# MCH matcher runtime benchmark (T29)",
        "",
        f"Generated by `dev_utils/mch_bench.py` on {time.strftime('%Y-%m-%d')}.",
        "",
        "Two runtimes timed over the parity-sweep spec table x the "
        "`tests/traces/` corpus: legacy hand-written matchers and the v2 IR "
        "engine. (The compiled frontend leg was deleted in T29 stage 2.) Fresh "
        "block tree per timed run; tree build and prereq passes excluded from "
        "the timed region; 2 warmup + 5 timed runs per (row, trace, mode), "
        "median of the 5. Times are median microseconds per trace across each "
        "row's candidate fixtures.",
        "",
        "| matcher | n_traces | legacy us | IR us | IR/legacy |",
        "|---|--:|--:|--:|--:|",
    ]
    for r in sorted(rows, key=lambda x: x["ir_ratio"], reverse=True):
        md.append(
            f"| {r['matcher']} | {r['n']} | {r['legacy_us']:.1f} | "
            f"{r['ir_us']:.1f} | {r['ir_ratio']:.2f} |"
        )
    md.append(
        f"| **AGGREGATE (sum-of-medians)** | | {agg['legacy']/1000.0:.1f} | "
        f"{agg['ir']/1000.0:.1f} | **{overall_ir:.2f}** |"
    )
    md += ["", "## Gate verdict", "",
           f"- Overall IR/legacy = **{overall_ir:.2f}x** -> **{gate}** (gate: <= ~1.5x)."]
    if skipped:
        md += ["", "## Skipped rows", ""]
        for k, why in skipped:
            md.append(f"- `{k}`: {why}")
    (_HERE / "mch_bench_results.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    print(f"\nWrote {_HERE / 'mch_bench_results.md'}")


if __name__ == "__main__":
    asyncio.run(main())
