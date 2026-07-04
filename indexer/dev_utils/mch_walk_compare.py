"""Compare the FINAL MATCHER TREE WALK of an mch-compiled matcher against the
legacy hand-written matcher it was ported from — traversal-level parity, not
just result parity.

It reuses the `tools/why_not.py` instrumentation wholesale: importing
`tools.matcher_trace` installs dormant class-level wrappers on every
BlockMatcher method (test_self / try_build / build_block / process_child_matcher
/ process_parent_matcher). Running a matcher inside `mt.record()` yields an
ordered event stream (each event carries matcher identity, block identity,
outcome, and a parent link). This script drives the SAME fixtures + tree-build +
prerequisite-replay the mch integration tests use (tests/mch/integration/*),
records both the legacy and the mch walk, canonicalises each event so the two
implementations are comparable (opcode/btype families instead of instance ids
and generated class names), and diffs the two walks.

Why canonicalisation is needed:
  * The mch anchor is a generated `_MCH_<name>` subclass; the legacy anchor is a
    hand-written class. Both are normalised to family ANCHOR.
  * `op X` sub-matchers are `ContractMatcher(opcode=X)` on both sides -> OP:X.
  * The mch `_branches_eligible_for_exclusive_heuristic` promotes some `(a | b)`
    alternatives to ExclusiveOrMatcher where the legacy uses OrMatcher. That is
    kept as a real family difference (OR vs XOR) so the diff surfaces it.

Two walk artifacts that are pure mch lowering side effects are handled explicitly
so they don't drown the structural diff:
  * The generated anchor overrides `try_build`, which calls `base.try_build`.
    That records TWO try_build events (override + base) for the same
    matcher+block. These base-wrapper events are folded away (and counted).
  * The generated `build_block` runs `_extract_captures`, which calls
    `test_self` all over the lowered tree to bind captures. Those probes live
    strictly under a `build_block` ancestor; they are split out as the
    "build/extraction phase" and excluded from the structural matching walk
    (but counted and reported).

Usage (from S:\\Dev\\ton-indexer-clear\\indexer, venv python):

    S:\\Dev\\H-dump\\tonscan-indexator\\venv0\\Scripts\\python.exe \
        dev_utils/mch_walk_compare.py

    # one scenario, full aligned side-by-side walk (not just divergent regions):
    python dev_utils/mch_walk_compare.py --only dedust_multihop --full

    # machine-readable dump of both walks + diff for every scenario:
    python dev_utils/mch_walk_compare.py --json > walks.json

Options:
    --only NAME       run a single scenario (see the SCENARIOS table below)
    --list            list scenario names and exit
    --full            print every step, not only divergent regions
    --show-build      also print the mch-only build/extraction probes per scenario
    --json            emit JSON (walks + per-event records + diff) to stdout
"""
from __future__ import annotations

import argparse
import asyncio
import difflib
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Awaitable, Callable

REPO_ROOT = Path(__file__).resolve().parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

try:
    sys.stdout.reconfigure(encoding="utf-8")
except (AttributeError, OSError):
    pass

# Break the circular import chain in indexer.events.context (same as why_not.py).
import indexer.core.database  # noqa: E402,F401
import indexer.events.context as context  # noqa: E402

# Importing this installs the dormant matcher patches used by why_not.
import tools.matcher_trace as mt  # noqa: E402
from tools.matcher_trace._graph import walk_matcher_graph  # noqa: E402

from indexer.events.blocks.basic_matchers import (  # noqa: E402
    BlockMatcher,
    BlockTypeMatcher,
    ContractMatcher,
    ExclusiveOrMatcher,
    GenericMatcher,
    OrMatcher,
    RecursiveMatcher,
    TonTransferMatcher,
)
from indexer.events.blocks.core import Block  # noqa: E402
from indexer.events.blocks.utils.tree_utils import to_tree  # noqa: E402
from indexer.events.event_processing import init_block  # noqa: E402
from indexer.events.mch import compile_file  # noqa: E402
from indexer.events.mch.builders import (  # noqa: E402
    coffee_registries,
    dedust_registries,
    dedust_withdraw_registries,
    dns_registries,
    jetton_registries,
)
import indexer.events.mch.specs as specs_pkg  # noqa: E402

from indexer.events.blocks.dns import ChangeDnsRecordMatcher  # noqa: E402
from indexer.events.blocks.jettons import (  # noqa: E402
    JettonBurnBlockMatcher,
    JettonMintBlockMatcher,
    JettonTransferBlockMatcher,
    PTonTransferMatcher,
)
from indexer.events.blocks.liquidity import DedustWithdrawBlockMatcher  # noqa: E402
from indexer.events.blocks.swaps import (  # noqa: E402
    CoffeeSwapBlockMatcher,
    DedustSwapBlockMatcher,
    ToncoSwapBlockMatcher,
)
from indexer.events.mch.builders.tonco_swap import tonco_swap_registries  # noqa: E402

from tests.utils.trace_deserializer import load_trace_from_file  # noqa: E402
from tests.utils.repository import TestInterfaceRepository  # noqa: E402
# Reuse the DNS synthetic-trace builders verbatim (no recorded fixture exists).
from tests.mch.integration.test_dns import (  # noqa: E402
    _build_dns_transactions,
    _interfaces as _dns_interfaces,
)
# Same for dedust withdraw (T17; corpus has no burn+payout trace).
from tests.mch.integration.test_dedust_withdraw import (  # noqa: E402
    _build_withdraw_transactions,
    _interfaces as _withdraw_interfaces,
)

SPECS_DIR = Path(specs_pkg.__file__).parent
TRACES_DIR = REPO_ROOT / "tests" / "traces"


# --------------------------------------------------------------------------- #
# tree building / matcher construction (mirrors tests/mch/integration/*)
# --------------------------------------------------------------------------- #

def _build_block_tree(txs) -> Block:
    node = to_tree(txs)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


def _top_level_blocks(root: Block) -> list[Block]:
    """Parent-less, non-root blocks driven by the anchor.

    Sorted deterministically. The two runs build independent trees, and the
    engine's `set()` use inside RecursiveMatcher/merge_blocks makes bfs order
    unstable across runs. That order is chosen by the DRIVER, not by matcher
    lowering, so pinning it keeps the walk comparison about lowering only.
    (event_processing drives a fixed BFS in production; this is a harness knob.)"""
    tops = [b for b in root.bfs_iter() if b.btype != "root" and b.parent is None]
    return sorted(tops, key=lambda b: (b.min_lt, tuple(sorted(
        n.message.tx_hash for n in b.event_nodes))))


async def _replay_prerequisites(root: Block, prereqs: list[BlockMatcher]) -> None:
    """Run prerequisite matchers to completion over the raw tree BEFORE recording,
    exactly like event_processing registration order. Their walk is not recorded."""
    for pre in prereqs:
        for b in _top_level_blocks(root):
            await pre.try_build(b)


def _compile_mch(spec_file: str, registries_factory, name: str) -> BlockMatcher:
    compiled = compile_file(SPECS_DIR / spec_file, registries_factory())
    by_name = {cm.name: cm.matcher for cm in compiled}
    return by_name[name]


# --------------------------------------------------------------------------- #
# scenario definitions
# --------------------------------------------------------------------------- #

@dataclass
class Setup:
    root: Block
    target: BlockMatcher
    tops: list[Block]


@dataclass
class Scenario:
    name: str
    protocol: str          # canonical anchor label shared by both impls
    prereqs: list[str]     # names, for reporting only
    make_legacy: Callable[[], Awaitable[Setup]]
    make_mch: Callable[[], Awaitable[Setup]]


def _trace_scenario_factories(trace_id, legacy_cls, mch_name, spec, reg, prereq_ctors):
    """Return (make_legacy, make_mch) that each build a FRESH tree from the same
    recorded trace, set the trace's interface bundle, replay prerequisites, and
    return the target matcher ready to drive."""

    async def _make(target_factory) -> Setup:
        trace, interfaces = load_trace_from_file(TRACES_DIR / f"{trace_id}.lz4")
        context.interface_repository.set(TestInterfaceRepository(interfaces or {}))
        root = _build_block_tree(trace.transactions)
        prereqs = [ctor() for ctor in prereq_ctors]
        await _replay_prerequisites(root, prereqs)
        target = target_factory()
        return Setup(root=root, target=target, tops=_top_level_blocks(root))

    async def make_legacy() -> Setup:
        return await _make(legacy_cls)

    async def make_mch() -> Setup:
        return await _make(lambda: _compile_mch(spec, reg, mch_name))

    return make_legacy, make_mch


def _dns_scenario_factories(outcome, with_bounce, aborted):
    async def _make(target_factory) -> Setup:
        context.interface_repository.set(TestInterfaceRepository(_dns_interfaces()))
        txs = _build_dns_transactions(outcome, with_bounce, aborted)
        root = _build_block_tree(txs)
        target = target_factory()
        return Setup(root=root, target=target, tops=_top_level_blocks(root))

    async def make_legacy() -> Setup:
        return await _make(ChangeDnsRecordMatcher)

    async def make_mch() -> Setup:
        return await _make(lambda: _compile_mch("dns.mch", dns_registries, "change_dns"))

    return make_legacy, make_mch


def _withdraw_scenario_factories(jetton_first: bool):
    async def _make(target_factory) -> Setup:
        context.interface_repository.set(TestInterfaceRepository(_withdraw_interfaces()))
        txs = _build_withdraw_transactions(jetton_first)
        root = _build_block_tree(txs)
        prereqs = [JettonTransferBlockMatcher(), PTonTransferMatcher()]
        await _replay_prerequisites(root, prereqs)
        target = target_factory()
        return Setup(root=root, target=target, tops=_top_level_blocks(root))

    async def make_legacy() -> Setup:
        return await _make(DedustWithdrawBlockMatcher)

    async def make_mch() -> Setup:
        return await _make(lambda: _compile_mch(
            "dedust_withdraw.mch", dedust_withdraw_registries, "dedust_withdraw"))

    return make_legacy, make_mch


def _build_scenarios() -> list[Scenario]:
    scen: list[Scenario] = []

    # ---- jettons (recorded fixtures; no prerequisites) ------------------- #
    jetton_cases = [
        ("jetton_transfer", "KYQfjYD1YybzQIuoJwkoXL-Y-apztmw6AGjIcPhotjQ=",
         JettonTransferBlockMatcher, "jetton_transfer"),
        ("jetton_burn", "xBl_8BTUqSv52IWAzrmg54-V0YR_WQsV1buJxKBq4vE=",
         JettonBurnBlockMatcher, "jetton_burn"),
        ("jetton_burn_lp", "b0QdK2TPX7Pv7NiXvpJxf_0PEumbW1FMewsnbFi-nqs=",
         JettonBurnBlockMatcher, "jetton_burn"),
        ("jetton_mint_minter_op", "SVotcOQBV7MZ-CdW7WsaOucRods9-SF0vnl646rxsBc=",
         JettonMintBlockMatcher, "jetton_mint"),
        ("jetton_mint_mint_op", "uXA5LQpf_4c9NEOK6trJZuBMitcBLNYxK7_2OfzphtI=",
         JettonMintBlockMatcher, "jetton_mint"),
        ("jetton_mint_failed", "MyrAUqqz_XH2D5bUdRd-YYCVo7JEEzjWX00gGXRFLLY=",
         JettonMintBlockMatcher, "jetton_mint"),
    ]
    for name, tid, legacy, mch_name in jetton_cases:
        ml, mm = _trace_scenario_factories(
            tid, legacy, mch_name, "jettons.mch", jetton_registries, []
        )
        scen.append(Scenario(name, mch_name, [], ml, mm))

    # ---- dns (synthetic trees) ------------------------------------------- #
    dns_cases = [
        ("dns_change", "change_dns", False, False),
        ("dns_renew", "renew_dns", False, False),
        ("dns_delete", "delete_dns", False, False),
        ("dns_change_bounce", "change_dns", True, False),
        ("dns_change_failed", "change_dns", True, True),
    ]
    for name, outcome, bounce, aborted in dns_cases:
        ml, mm = _dns_scenario_factories(outcome, bounce, aborted)
        scen.append(Scenario(name, "change_dns", [], ml, mm))

    # ---- dedust (recorded fixtures; jetton/pton prerequisites) ----------- #
    dedust_prereqs = [JettonTransferBlockMatcher, PTonTransferMatcher]
    dedust_cases = [
        ("dedust_multihop", "C7bHOfSrBJPLHXMZkVnIQladzgYZHIlKOiJRvbxRRuI="),
        ("dedust_multihop_2", "c8jOrHqyuJe1hMGx4fm1BTGKXgBQ5O35rbExCkhmRG8="),
        ("dedust_single_hop_ton_in", "cq7UujFWNfSnpHdRKseD41yHqj4Yx3SCWsnkw8IRPoU="),
        ("dedust_reject_no_pool", "7bcODUs7IVJBjdApVMCJyUKoztjS_k4msfHyYOwt3qk="),
    ]
    for name, tid in dedust_cases:
        ml, mm = _trace_scenario_factories(
            tid, DedustSwapBlockMatcher, "dedust_swap", "dedust_swap.mch",
            dedust_registries, dedust_prereqs,
        )
        scen.append(Scenario(name, "dedust_swap", ["jetton_transfer", "pton_transfer"], ml, mm))

    # ---- dedust withdraw (synthetic trees; jetton/pton prerequisites) ---- #
    for name, jetton_first in (("dedust_withdraw_jetton_first", True),
                               ("dedust_withdraw_ton_first", False)):
        ml, mm = _withdraw_scenario_factories(jetton_first)
        scen.append(Scenario(name, "dedust_withdraw",
                             ["jetton_transfer", "pton_transfer"], ml, mm))

    # ---- coffee (recorded fixtures; jetton/pton prerequisites) ----------- #
    coffee_cases = [
        ("coffee_multihop", "dp1xdxzIC7tCyZfy80VTW5be7SimpOOTnuf3_8BZjb0="),
        ("coffee_single_hop_ton_out", "p1-SZ1wV7yEe2qf1SQujTW5OMifEPuuX-jPPrPNiKxw="),
        ("coffee_native_ton_in", "cq7UujFWNfSnpHdRKseD41yHqj4Yx3SCWsnkw8IRPoU="),
    ]
    for name, tid in coffee_cases:
        ml, mm = _trace_scenario_factories(
            tid, CoffeeSwapBlockMatcher, "coffee_swap", "coffee_swap.mch",
            coffee_registries, dedust_prereqs,
        )
        scen.append(Scenario(name, "coffee_swap", ["jetton_transfer", "pton_transfer"], ml, mm))

    # ---- tonco (R11/T28, cyclic recursion strategy; jetton/pton prereqs) - #
    # NOTE: legacy tonco is label-heavy (LabeledBlockConstructorMatcher flattens
    # into lambda test_selfs), so walk-level identity is structurally
    # unattainable (T17/T19 note) — use L_only/M_only, not the ratio.
    tonco_cases = [
        ("tonco_single_hop", "g1JLQayzzERU9tojKPorN9Asd-MCF_0omnRkwOd_Olo="),
        ("tonco_multihop", "CjtIXjqazbSFrtPqqMMFBgMG6-huwp5CfNtXpQAf8xY="),
        ("tonco_failed_swap", "02TsVcC_MOggAZvHHBITbHCvzAt_hCq5WVAiKcZFMno="),
    ]
    for name, tid in tonco_cases:
        ml, mm = _trace_scenario_factories(
            tid, ToncoSwapBlockMatcher, "tonco_swap", "tonco_swap.mch",
            tonco_swap_registries, dedust_prereqs,
        )
        scen.append(Scenario(name, "tonco_swap", ["jetton_transfer", "pton_transfer"], ml, mm))

    return scen


# --------------------------------------------------------------------------- #
# canonicalisation of a matcher instance to an impl-neutral (family, param)
# --------------------------------------------------------------------------- #

def _matcher_kind(m: BlockMatcher) -> tuple[str, object]:
    # order matters: ExclusiveOr before Or (subclass); _Mch* Recursive covered by base
    if isinstance(m, ExclusiveOrMatcher):
        return ("XOR", len(m.matchers))
    if isinstance(m, OrMatcher):
        return ("OR", len(m.matchers))
    if isinstance(m, RecursiveMatcher):
        return ("REC", None)
    if isinstance(m, ContractMatcher):
        op = getattr(m, "opcode", None)
        return ("OP", f"0x{op & 0xFFFFFFFF:08x}" if isinstance(op, int) else str(op))
    if isinstance(m, BlockTypeMatcher):
        return ("BTYPE", getattr(m, "block_type", None))
    if isinstance(m, TonTransferMatcher):
        return ("TON", None)
    if isinstance(m, GenericMatcher):
        fn = getattr(m, "test_self_func", None)
        return ("GENERIC", getattr(fn, "__name__", str(fn)))
    return ("MATCHER", type(m).__name__)


_SLOT_OF_ROLE = {
    "primary": "root",
    "child_of": "child",
    "parent_of": "parent",
    "ordered_child": "children[]",
    "or_branch": "or",
    "recursive_repeat": "rec.step",
    "recursive_exit": "rec.exit",
}


def _classify_tree(target: BlockMatcher, protocol: str) -> dict[int, dict]:
    """id(matcher) -> {family, param, slot}. Slots come from walk_matcher_graph,
    normalised to drop the (impl-specific) parent class name. The target itself
    is forced to family ANCHOR/param protocol so legacy and mch anchors align."""
    info: dict[int, dict] = {}
    for m, role in walk_matcher_graph([target]):
        fam, param = _matcher_kind(m)
        slot = _SLOT_OF_ROLE.get(role.split(":", 1)[0], role.split(":", 1)[0])
        info[id(m)] = {"family": fam, "param": param, "slot": slot}
    info[id(target)] = {"family": "ANCHOR", "param": protocol, "slot": "root"}
    return info


# --------------------------------------------------------------------------- #
# fold the recording into a normalised, depth-annotated walk
# --------------------------------------------------------------------------- #

@dataclass
class Step:
    ev_id: int
    depth: int
    method: str
    family: str
    param: object
    slot: str
    block: str
    btype: str | None
    result: str
    phase: str          # "match" | "build"

    def token(self) -> str:
        return f"{self.depth}|{self.method}|{self.family}:{self.param}|{self.slot}|{self.block}|{self.result}"

    def render(self) -> str:
        pad = "  " * self.depth
        return f"{pad}{self.method} {self.family}:{self.param} [{self.slot}] {self.block} -> {self.result}"


@dataclass
class Walk:
    steps: list[Step]
    folded_wrapper_try_builds: int
    method_counts: dict[str, int] = field(default_factory=dict)


def _build_walk(rec: "mt.Recording", kinds: dict[int, dict]) -> Walk:
    events = sorted(rec.events, key=lambda e: e.id)
    ev_by_id = {e.id: e for e in events}

    # A generated anchor's try_build override calls base.try_build -> a second
    # try_build event for the same matcher+block nested directly inside. Fold it.
    is_fold: dict[int, bool] = {}
    for e in events:
        p = ev_by_id.get(e.parent_id) if e.parent_id is not None else None
        is_fold[e.id] = bool(
            e.method == "try_build"
            and p is not None
            and p.method == "try_build"
            and p.matcher_id == e.matcher_id
            and p.block_id == e.block_id
        )

    def eff_parent_id(e) -> int | None:
        p = e.parent_id
        while p is not None and is_fold.get(p):
            p = ev_by_id[p].parent_id
        return p

    def depth_of(e) -> int:
        d = 0
        pid = eff_parent_id(e)
        while pid is not None:
            d += 1
            pid = eff_parent_id(ev_by_id[pid])
        return d

    def phase_of(e) -> str:
        pid = eff_parent_id(e)
        while pid is not None:
            if ev_by_id[pid].method == "build_block":
                return "build"
            pid = eff_parent_id(ev_by_id[pid])
        return "match"

    steps: list[Step] = []
    method_counts: dict[str, int] = {}
    for e in events:
        if is_fold[e.id]:
            continue
        k = kinds.get(e.matcher_id, {"family": "?", "param": type(e).__name__, "slot": "?"})
        steps.append(Step(
            ev_id=e.id,
            depth=depth_of(e),
            method=e.method,
            family=k["family"],
            param=k["param"],
            slot=k["slot"],
            block=e.block_summary,
            btype=e.block_btype,
            result=e.result_summary,
            phase=phase_of(e),
        ))
        method_counts[e.method] = method_counts.get(e.method, 0) + 1

    folded = sum(1 for v in is_fold.values() if v)
    return Walk(steps=steps, folded_wrapper_try_builds=folded, method_counts=method_counts)


# --------------------------------------------------------------------------- #
# run one implementation and record its walk
# --------------------------------------------------------------------------- #

async def _record_walk(setup: Setup, protocol: str) -> Walk:
    kinds = _classify_tree(setup.target, protocol)
    async with mt.record() as rec:
        for b in setup.tops:
            await setup.target.try_build(b)
    return _build_walk(rec, kinds)


# --------------------------------------------------------------------------- #
# diffing
# --------------------------------------------------------------------------- #

@dataclass
class DiffResult:
    legacy: Walk
    mch: Walk
    match_ratio: float
    n_equal: int
    n_replace: int
    n_delete: int   # legacy-only steps
    n_insert: int   # mch-only steps
    or_xor_swaps: int
    opcodes: list


def _match_steps(walk: Walk) -> list[Step]:
    return [s for s in walk.steps if s.phase == "match"]


def _diff_walks(legacy: Walk, mch: Walk) -> DiffResult:
    a = _match_steps(legacy)
    b = _match_steps(mch)
    at = [s.token() for s in a]
    bt = [s.token() for s in b]
    sm = difflib.SequenceMatcher(a=at, b=bt, autojunk=False)
    n_eq = n_rep = n_del = n_ins = 0
    or_xor = 0
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            n_eq += (i2 - i1)
        elif tag == "replace":
            n_rep += max(i2 - i1, j2 - j1)
            # count OR<->XOR family swaps within the replaced region (paired)
            for la, lb in zip(a[i1:i2], b[j1:j2]):
                fams = {la.family, lb.family}
                if fams == {"OR", "XOR"} and la.method == lb.method and la.block == lb.block:
                    or_xor += 1
        elif tag == "delete":
            n_del += (i2 - i1)
        elif tag == "insert":
            n_ins += (j2 - j1)
    return DiffResult(
        legacy=legacy, mch=mch,
        match_ratio=sm.ratio(),
        n_equal=n_eq, n_replace=n_rep, n_delete=n_del, n_insert=n_ins,
        or_xor_swaps=or_xor,
        opcodes=sm.get_opcodes(),
    )


# --------------------------------------------------------------------------- #
# rendering
# --------------------------------------------------------------------------- #

def _count_phase(walk: Walk, method: str, phase: str) -> int:
    return sum(1 for s in walk.steps if s.method == method and s.phase == phase)


def _render_scenario(scn: Scenario, diff: DiffResult, *, full: bool, show_build: bool) -> str:
    a = _match_steps(diff.legacy)
    b = _match_steps(diff.mch)
    out: list[str] = []
    verdict = "IDENTICAL" if (diff.n_replace or diff.n_delete or diff.n_insert) == 0 else "DIVERGES"
    out.append(f"=== {scn.name}  [{verdict}]  ratio={diff.match_ratio:.3f}")
    if scn.prereqs:
        out.append(f"    prerequisites replayed (unrecorded): {', '.join(scn.prereqs)}")

    # counts table (matching phase)
    def row(label, la, mb):
        flag = "" if la == mb else "   <-- differs"
        return f"    {label:<26} legacy={la:<5} mch={mb}{flag}"

    out.append("    -- matching-phase event counts --")
    for meth in ("try_build", "test_self", "process_child_matcher",
                 "process_parent_matcher", "build_block"):
        out.append(row(meth, _count_phase(diff.legacy, meth, "match"),
                       _count_phase(diff.mch, meth, "match")))
    out.append(f"    {'matching steps (total)':<26} legacy={len(a):<5} mch={len(b)}")
    out.append("    -- mch lowering artifacts --")
    out.append(f"    {'folded anchor try_build':<26} legacy={diff.legacy.folded_wrapper_try_builds:<5} "
               f"mch={diff.mch.folded_wrapper_try_builds}")
    out.append(f"    {'build/extraction probes':<26} legacy="
               f"{sum(1 for s in diff.legacy.steps if s.phase == 'build'):<5} "
               f"mch={sum(1 for s in diff.mch.steps if s.phase == 'build')}")
    out.append("    -- structural diff (matching phase) --")
    out.append(f"    equal={diff.n_equal} replace={diff.n_replace} "
               f"legacy_only={diff.n_delete} mch_only={diff.n_insert} or_xor_swaps={diff.or_xor_swaps}")

    # aligned side-by-side of divergent regions (or everything if --full)
    for tag, i1, i2, j1, j2 in diff.opcodes:
        if tag == "equal" and not full:
            if i2 - i1 > 0:
                out.append(f"      = {i2 - i1} equal steps")
            continue
        if tag == "equal":
            for s in a[i1:i2]:
                out.append(f"      = {s.render()}")
            continue
        out.append(f"      ~ {tag}:")
        for s in a[i1:i2]:
            out.append(f"      L {s.render()}")
        for s in b[j1:j2]:
            out.append(f"      M {s.render()}")

    if show_build:
        mb = [s for s in diff.mch.steps if s.phase == "build"]
        if mb:
            out.append("    -- mch build/extraction probes (mch-only) --")
            for s in mb:
                out.append(f"      B {s.render()}")

    return "\n".join(out)


# --------------------------------------------------------------------------- #
# main
# --------------------------------------------------------------------------- #

async def _run(scenarios: list[Scenario]) -> list[tuple[Scenario, DiffResult]]:
    results = []
    for scn in scenarios:
        legacy_walk = await _record_walk(await scn.make_legacy(), scn.protocol)
        mch_walk = await _record_walk(await scn.make_mch(), scn.protocol)
        results.append((scn, _diff_walks(legacy_walk, mch_walk)))
    return results


def _walk_to_json(walk: Walk) -> dict:
    return {
        "folded_anchor_try_builds": walk.folded_wrapper_try_builds,
        "steps": [
            {
                "depth": s.depth, "method": s.method, "family": s.family,
                "param": s.param, "slot": s.slot, "block": s.block,
                "result": s.result, "phase": s.phase,
            }
            for s in walk.steps
        ],
    }


def main() -> None:
    scenarios = _build_scenarios()
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--only", help="run a single scenario by name")
    parser.add_argument("--list", action="store_true", help="list scenario names and exit")
    parser.add_argument("--full", action="store_true", help="print all steps, not just diffs")
    parser.add_argument("--show-build", action="store_true",
                        help="also print the mch-only build/extraction probes")
    parser.add_argument("--json", action="store_true", help="emit JSON to stdout")
    args = parser.parse_args()

    if args.list:
        for s in scenarios:
            print(s.name)
        return

    if args.only:
        scenarios = [s for s in scenarios if s.name == args.only]
        if not scenarios:
            raise SystemExit(f"no scenario named {args.only!r}; use --list")

    results = asyncio.run(_run(scenarios))

    if args.json:
        payload = []
        for scn, diff in results:
            payload.append({
                "scenario": scn.name,
                "protocol": scn.protocol,
                "match_ratio": diff.match_ratio,
                "counts": {
                    "equal": diff.n_equal, "replace": diff.n_replace,
                    "legacy_only": diff.n_delete, "mch_only": diff.n_insert,
                    "or_xor_swaps": diff.or_xor_swaps,
                },
                "legacy_walk": _walk_to_json(diff.legacy),
                "mch_walk": _walk_to_json(diff.mch),
            })
        json.dump(payload, sys.stdout, indent=2, default=str)
        sys.stdout.write("\n")
        return

    for scn, diff in results:
        print(_render_scenario(scn, diff, full=args.full, show_build=args.show_build))
        print()

    # roll-up
    print("=== ROLL-UP ===")
    for scn, diff in results:
        v = "IDENTICAL" if (diff.n_replace + diff.n_delete + diff.n_insert) == 0 else "DIVERGES"
        print(f"  {scn.name:<28} {v:<10} ratio={diff.match_ratio:.3f} "
              f"replace={diff.n_replace} L_only={diff.n_delete} M_only={diff.n_insert} "
              f"or_xor={diff.or_xor_swaps} "
              f"build_probes(mch)={sum(1 for s in diff.mch.steps if s.phase == 'build')}")


if __name__ == "__main__":
    main()
