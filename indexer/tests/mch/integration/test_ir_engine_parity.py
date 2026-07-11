"""CONFORMANCE SNAPSHOTS for the Python IR engine (indexer.events.mch_ir).

T29 stage 1 retired the compiled path, so the old compiled-vs-IR round-trip
comparison this file used to run is gone. What is worth keeping is a pin on the
IR engine's OWN output: for each covered fixture we build the matchers through
the engine (compile the spec -> emit_ir -> load_artifact -> build_matchers — the
compile step is a frontend-internal detail ir_emit needs, no compiled matcher is
run), run them over a real trace, and snapshot the produced blocks into a
checked-in JSON file under `snapshots/`. The test asserts the current output
equals the snapshot. (Legacy-vs-IR field parity for these same protocols is the
job of the per-protocol tests and test_parity_sweep.py; this file guards against
silent drift in the engine's block construction.)

Snapshot shape, per produced block (sorted deterministically):
    btype, failed, broken, sorted consumed event-node ids (tx hashes), and a
    canonicalized `data` repr. Canonicalization is DETERMINISTIC ACROSS
    PROCESSES: every leaf becomes its `repr()`, every dict is keyed by string,
    and every list is sorted as a MULTISET (by canonical JSON) — because some
    block fields inherit set()-iteration order from the legacy pipeline
    (compact_connections rebuilds next_blocks via list(set(...)), so
    order-bearing collections like dedust `vault_excesses` flip between runs).
    Nothing in a snapshot may depend on object identity or set order.

Regenerating snapshots:
    Set MCH_SNAPSHOT_REGEN=1 to rewrite every snapshot from the current engine
    output instead of asserting against it, e.g.

        MCH_SNAPSHOT_REGEN=1 python -m pytest \
            tests/mch/integration/test_ir_engine_parity.py

    Review the resulting JSON diff before committing — a snapshot change is a
    change in engine output and must be intentional.

The negative controls at the bottom are IR-engine behavior tests (version
mismatch, unknown predicate, unbound name), not round trips; they build an
artifact via emit_ir then mutate it, which never runs a compiled matcher.
"""
from __future__ import annotations

import dataclasses
import json
import os
from pathlib import Path

import pytest

from indexer.events import context
from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.jettons import (
    JettonTransferBlockMatcher,
    PTonTransferMatcher,
)
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
import indexer.events.mch as mch
from indexer.events.mch import compile_file
from indexer.events.mch.builders import (
    dedust_hybrid_registries,
    jetton_decl_registries,
    jetton_registries,
)
from indexer.events.mch.ir_emit import emit_ir, render_json
import indexer.events.mch.specs as specs_pkg
from tests.utils.repository import TestInterfaceRepository
from tests.utils.trace_deserializer import load_trace_from_file

# --- IR engine gate (teammate's package; may be mid-rewrite) ----------------
try:
    from indexer.events import mch_ir as _mch_ir

    _HAVE_IR = hasattr(_mch_ir, "load_artifact") and hasattr(_mch_ir, "build_matchers")
except Exception:  # pragma: no cover - import error path
    _mch_ir = None
    _HAVE_IR = False

_SKIP_REASON = "indexer.events.mch_ir not importable; the IR engine is required to snapshot"
_REGEN = os.environ.get("MCH_SNAPSHOT_REGEN") == "1"

# --- spec files -------------------------------------------------------------
JETTONS_DECL_MCH = Path(mch.__file__).parent / "specs_decl" / "jettons.mch"
DEDUST_HYBRID_MCH = Path(mch.__file__).parent / "specs_decl" / "dedust_swap_hybrid.mch"
JETTONS_BUILDER_MCH = Path(specs_pkg.__file__).parent / "jettons.mch"

_SNAP_DIR = Path(__file__).parent / "snapshots"


# ---------------------------------------------------------------------------
# Harness — same BFS + tree-construction prelude as the other integration tests.
# ---------------------------------------------------------------------------
def _load_trace(traces_dir: Path, filename: str):
    trace, interfaces = load_trace_from_file(traces_dir / filename)
    context.interface_repository.set(TestInterfaceRepository(interfaces))
    return trace


def _build_block_tree(trace) -> Block:
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matcher(matcher: BlockMatcher, trace, prereqs=()) -> list:
    root = _build_block_tree(trace)
    for pre in prereqs:
        for b in root.bfs_iter():
            if b.parent is None:
                await pre.try_build(b)
    matches: list = []
    for b in root.bfs_iter():
        if b.btype == "root" or b.parent is not None:
            continue
        result = await matcher.try_build(b)
        if result is not None:
            matches.extend(result)
    return matches


def _node_hashes(block: Block) -> list[str]:
    return sorted(n.message.tx_hash for n in block.event_nodes)


def _data_items(data) -> dict:
    if data is None:
        return {}
    if isinstance(data, dict):
        return {str(k): v for k, v in data.items()}
    if dataclasses.is_dataclass(data) and not isinstance(data, type):
        return {f.name: getattr(data, f.name) for f in dataclasses.fields(data)}
    return {"<data>": data}


def _canon(v):
    """Deterministic, JSON-serializable canonical form. Leaves -> repr strings;
    dicts -> string-keyed; lists -> multiset sorted by canonical JSON (order is
    never a snapshot property, see module docstring)."""
    if isinstance(v, dict):
        return {str(k): _canon(val) for k, val in v.items()}
    if dataclasses.is_dataclass(v) and not isinstance(v, type):
        return {f.name: _canon(getattr(v, f.name)) for f in dataclasses.fields(v)}
    if isinstance(v, (list, tuple)):
        items = [_canon(x) for x in v]
        return sorted(items, key=lambda x: json.dumps(x, sort_keys=True))
    return repr(v)


def _snapshot(blocks: list[Block]) -> list[dict]:
    snaps = []
    for b in blocks:
        snaps.append({
            "btype": b.btype,
            "failed": bool(b.failed),
            "broken": bool(b.broken),
            "consumed": _node_hashes(b),
            "data": _canon(_data_items(b.data)),
        })
    # Block order is not a property either — sort the snapshot list itself.
    return sorted(snaps, key=lambda s: json.dumps(s, sort_keys=True))


def _conform(snap_key: str, blocks: list[Block]) -> None:
    """Assert the produced blocks match the checked-in snapshot, or (under
    MCH_SNAPSHOT_REGEN=1) rewrite it."""
    produced = _snapshot(blocks)
    path = _SNAP_DIR / f"{snap_key}.json"
    if _REGEN:
        _SNAP_DIR.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(produced, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return
    assert path.exists(), (
        f"snapshot {path.name} missing — regenerate with "
        f"MCH_SNAPSHOT_REGEN=1 python -m pytest {Path(__file__).name}"
    )
    expected = json.loads(path.read_text(encoding="utf-8"))
    assert produced == expected, (
        f"IR engine output drifted from snapshot {path.name}.\n"
        f"expected={json.dumps(expected, sort_keys=True)}\n"
        f"produced={json.dumps(produced, sort_keys=True)}\n"
        f"If this change is intentional, regenerate with MCH_SNAPSHOT_REGEN=1."
    )


async def _ir_blocks(spec_path: Path, registries_factory, name: str,
                     target_btype: str, trace, prereqs=()) -> list[Block]:
    """Build the `name` matcher through the IR engine and return its produced
    blocks of `target_btype`. Emits + renders the artifact first so a break in
    the emit plumbing errors here rather than skipping."""
    compiled = compile_file(spec_path, registries_factory())
    artifact = emit_ir(compiled, [spec_path.name])
    render_json(artifact)  # emit path must not raise
    art = _mch_ir.load_artifact(artifact)
    ir_by_name = {m.name: m for m in _mch_ir.build_matchers(art, registries_factory())}
    assert name in ir_by_name, f"{name!r} not built by the IR engine from {spec_path.name}"
    return [b for b in await _run_matcher(ir_by_name[name], trace, prereqs)
            if b.btype == target_btype]


# ---------------------------------------------------------------------------
# Tier 1 — fully declarative jettons (specs_decl/jettons.mch). Same 6 fixtures
# as test_jettons_decl.py.
# ---------------------------------------------------------------------------
JETTON_CASES = [
    ("jetton_transfer", "KYQfjYD1YybzQIuoJwkoXL-Y-apztmw6AGjIcPhotjQ=.lz4"),
    ("jetton_burn", "xBl_8BTUqSv52IWAzrmg54-V0YR_WQsV1buJxKBq4vE=.lz4"),
    ("jetton_burn", "b0QdK2TPX7Pv7NiXvpJxf_0PEumbW1FMewsnbFi-nqs=.lz4"),
    ("jetton_mint", "SVotcOQBV7MZ-CdW7WsaOucRods9-SF0vnl646rxsBc=.lz4"),
    ("jetton_mint", "uXA5LQpf_4c9NEOK6trJZuBMitcBLNYxK7_2OfzphtI=.lz4"),
    ("jetton_mint", "MyrAUqqz_XH2D5bUdRd-YYCVo7JEEzjWX00gGXRFLLY=.lz4"),
]
_JETTON_IDS = [
    "jetton_transfer",
    "jetton_burn-plain",
    "jetton_burn-lp",
    "jetton_mint-0x15",
    "jetton_mint-0x642b7d07",
    "jetton_mint-failed",
]


@pytest.mark.skipif(not _HAVE_IR, reason=_SKIP_REASON)
@pytest.mark.asyncio
@pytest.mark.parametrize("name,filename", JETTON_CASES, ids=_JETTON_IDS)
async def test_tier1_declarative_jettons_snapshot(traces_dir, name, filename):
    trace = _load_trace(traces_dir, filename)
    blocks = await _ir_blocks(JETTONS_DECL_MCH, jetton_decl_registries, name, name, trace)
    _conform(f"tier1_decl_{name}_{filename[:-4]}", blocks)


# ---------------------------------------------------------------------------
# Tier 2 — dedust hybrid (specs_decl/dedust_swap_hybrid.mch). Same 4 fixtures as
# test_dedust_hybrid.py; the last rejects (pool data absent) -> empty snapshot.
# Legacy JettonTransfer + PTon prerequisites prepare the tree.
# ---------------------------------------------------------------------------
DEDUST_CASES = [
    ("C7bHOfSrBJPLHXMZkVnIQladzgYZHIlKOiJRvbxRRuI=.lz4", "multihop"),
    ("c8jOrHqyuJe1hMGx4fm1BTGKXgBQ5O35rbExCkhmRG8=.lz4", "multihop-2"),
    ("cq7UujFWNfSnpHdRKseD41yHqj4Yx3SCWsnkw8IRPoU=.lz4", "single_hop_ton_in"),
    ("7bcODUs7IVJBjdApVMCJyUKoztjS_k4msfHyYOwt3qk=.lz4", "reject-missing-pool"),
]


@pytest.mark.skipif(not _HAVE_IR, reason=_SKIP_REASON)
@pytest.mark.asyncio
@pytest.mark.parametrize("filename,case_id", DEDUST_CASES, ids=[c[1] for c in DEDUST_CASES])
async def test_tier2_dedust_hybrid_snapshot(traces_dir, filename, case_id):
    trace = _load_trace(traces_dir, filename)
    prereqs = [JettonTransferBlockMatcher(), PTonTransferMatcher()]
    blocks = await _ir_blocks(
        DEDUST_HYBRID_MCH, dedust_hybrid_registries, "dedust_swap_hybrid",
        "jetton_swap", trace, prereqs=prereqs,
    )
    _conform(f"tier2_dedust_hybrid_{case_id}", blocks)


# ---------------------------------------------------------------------------
# Tier 3 — builder-based jettons (specs/jettons.mch). Same 6 fixtures as
# test_jettons_family.py, real builder registries.
# ---------------------------------------------------------------------------
@pytest.mark.skipif(not _HAVE_IR, reason=_SKIP_REASON)
@pytest.mark.asyncio
@pytest.mark.parametrize("name,filename", JETTON_CASES, ids=_JETTON_IDS)
async def test_tier3_builder_jettons_snapshot(traces_dir, name, filename):
    trace = _load_trace(traces_dir, filename)
    blocks = await _ir_blocks(JETTONS_BUILDER_MCH, jetton_registries, name, name, trace)
    _conform(f"tier3_builder_{name}_{filename[:-4]}", blocks)


# ---------------------------------------------------------------------------
# Tier 4 — negative controls (IR-engine behavior, not round trips).
# ---------------------------------------------------------------------------
@pytest.mark.skipif(not _HAVE_IR, reason=_SKIP_REASON)
def test_neg_version_mismatch_rejected():
    """load_artifact is version-checked (schema: mch_ir_version == '1.0')."""
    compiled = compile_file(JETTONS_DECL_MCH, jetton_decl_registries())
    artifact = emit_ir(compiled, [JETTONS_DECL_MCH.name])
    artifact["mch_ir_version"] = "9.9-not-a-real-version"
    with pytest.raises(Exception):
        _mch_ir.load_artifact(artifact)


@pytest.mark.skipif(not _HAVE_IR, reason=_SKIP_REASON)
def test_neg_unknown_predicate_rejected():
    """A node referencing a predicate name absent from the registries must fail
    to build."""
    compiled = compile_file(JETTONS_DECL_MCH, jetton_decl_registries())
    artifact = emit_ir(compiled, [JETTONS_DECL_MCH.name])
    root_id = artifact["matchers"][0]["root"]
    artifact["nodes"][root_id]["where"] = "__unregistered_predicate__"
    with pytest.raises(Exception):
        art = _mch_ir.load_artifact(artifact)
        _mch_ir.build_matchers(art, jetton_decl_registries())


@pytest.mark.skipif(not _HAVE_IR, reason=_SKIP_REASON)
@pytest.mark.asyncio
async def test_neg_unbound_name_in_build_program(traces_dir):
    """A build program referencing a name that is neither a capture nor a `let`
    must surface a clear runtime error (a raise at load/build/run OR a clean
    rejection [] both count, per spec §10.7)."""
    compiled = compile_file(JETTONS_DECL_MCH, jetton_decl_registries())
    artifact = emit_ir(compiled, [JETTONS_DECL_MCH.name])
    transfer = next(m for m in artifact["matchers"] if m["name"] == "jetton_transfer")
    for stmt in transfer.get("build_program", []):
        if stmt.get("s") == "out":
            for field in stmt["fields"]:
                if field["name"] == "sender":
                    field["expr"] = {"k": "name", "id": "__definitely_unbound__"}

    trace = _load_trace(traces_dir, JETTON_CASES[0][1])
    try:
        art = _mch_ir.load_artifact(artifact)
        ir_matchers = _mch_ir.build_matchers(art, jetton_decl_registries())
        ir_matcher = {m.name: m for m in ir_matchers}["jetton_transfer"]
        produced = [
            b for b in await _run_matcher(ir_matcher, trace)
            if b.btype == "jetton_transfer"
        ]
    except Exception:
        return
    assert produced == [], (
        "unbound name in build program neither raised nor rejected the match"
    )
