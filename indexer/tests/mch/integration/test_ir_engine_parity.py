"""ROUND-TRIP parity: the Python IR engine (indexer.events.mch_ir) reproduces the
compiled `.mch` matchers exactly.

For one spec + one trace fixture, two matcher graphs are built:

    compiled  =  [.mch  -> compile_file]                 (indexer.events.mch)
    ir        =  [.mch  -> emit_ir -> load_artifact -> build_matchers]

Both run over independently built block trees and every produced block is
compared field by field at the SAME bar as the existing compiled-vs-legacy
parity tests (test_jettons_decl / test_jettons_family / test_dedust_hybrid):
btype, deep `data` equality (per key), `failed`/`broken` flags,
`value_flow.to_dict()`, and the consumed event-node set (sorted tx hashes).

The reference side is the COMPILED path, not the legacy matcher — this isolates
the IR round trip (emit -> load -> build) from any compiled-vs-legacy question,
which the other files already cover.

Sequencing: the IR engine package (indexer.events.mch_ir) is built in parallel.
Until it is importable, the round-trip comparison is SKIPPED at the point it
needs the engine — but every test still exercises the runnable-now half first
(compile the spec, run the compiled matcher, emit the IR artifact, render it to
JSON), so a break in that plumbing ERRORS here rather than silently skipping.
See `_HAVE_IR` / `_SKIP_REASON` below.

Tier 2 (dedust) prerequisite mix: the dedust pattern matches on already-built
`jetton_transfer` blocks, so the legacy JettonTransfer + PTon matchers run first
to prepare the tree — exactly as test_dedust_hybrid.py does. These prerequisites
are LEGACY (there is no pton `.mch` spec to round-trip), and they run
identically on both the compiled tree and the IR tree, so they are neutral tree
preparation: the comparison still isolates the dedust matcher (compiled vs IR).
"""
from __future__ import annotations

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

# --- IR engine gate (teammate's package; may not exist yet) -----------------
try:
    from indexer.events import mch_ir as _mch_ir

    _HAVE_IR = hasattr(_mch_ir, "load_artifact") and hasattr(_mch_ir, "build_matchers")
except Exception:  # pragma: no cover - import error path
    _mch_ir = None
    _HAVE_IR = False

_SKIP_REASON = (
    "indexer.events.mch_ir not importable yet (built in parallel); the "
    "compiled-path run + IR emit above still ran"
)

# --- spec files -------------------------------------------------------------
JETTONS_DECL_MCH = Path(mch.__file__).parent / "specs_decl" / "jettons.mch"
DEDUST_HYBRID_MCH = Path(mch.__file__).parent / "specs_decl" / "dedust_swap_hybrid.mch"
JETTONS_BUILDER_MCH = Path(specs_pkg.__file__).parent / "jettons.mch"


# ---------------------------------------------------------------------------
# Harness — copied from the existing integration parity tests so the same BFS +
# tree-construction prelude runs unchanged against IR matchers (they expose the
# same BlockMatcher interface: test_self / try_build returning [produced_block]).
# ---------------------------------------------------------------------------
def _load_trace(traces_dir: Path, filename: str):
    """Same setup as the `trace_data` fixture: load + publish the interface repo."""
    trace, interfaces = load_trace_from_file(traces_dir / filename)
    context.interface_repository.set(TestInterfaceRepository(interfaces))
    return trace


def _build_block_tree(trace) -> Block:
    """Replicate event_processing's prelude: EventNode tree -> Block tree."""
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matcher(matcher: BlockMatcher, trace, prereqs=()) -> list:
    """Run a matcher across a fresh block tree, mirroring process_event_async.

    `prereqs` are matchers applied first to prepare the tree (dedust needs
    jetton_transfer blocks built before it can match).
    """
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


def _values_equal(a, b) -> bool:
    if a is None and b is None:
        return True
    if type(a) is not type(b):
        return False
    return bool(a == b)


def _node_hashes(block: Block) -> list[str]:
    return sorted(n.message.tx_hash for n in block.event_nodes)


def _sort_key(block: Block):
    return (block.min_lt, tuple(_node_hashes(block)))


def _assert_roundtrip_parity(
    expected: list[Block],
    actual: list[Block],
    btype: str,
    *,
    allow_empty: bool = False,
) -> None:
    """Field-level equality between the compiled blocks (`expected`) and the IR
    blocks (`actual`) — same depth as the existing parity assertions."""
    if not allow_empty:
        assert len(expected) > 0, (
            f"compiled path produced no {btype} blocks - stale fixture"
        )
    assert len(actual) == len(expected), (
        f"count mismatch: ir={len(actual)} compiled={len(expected)}"
    )
    for a, e in zip(sorted(actual, key=_sort_key), sorted(expected, key=_sort_key)):
        assert a.btype == e.btype == btype
        assert a.failed == e.failed, f"failed flag: ir={a.failed} compiled={e.failed}"
        assert a.broken == e.broken, f"broken flag: ir={a.broken} compiled={e.broken}"

        assert set(a.data.keys()) == set(e.data.keys()), (
            f"data keys: ir={sorted(a.data)} compiled={sorted(e.data)}"
        )
        for key in sorted(e.data):
            av, ev = a.data[key], e.data[key]
            assert _values_equal(av, ev), (
                f"data[{key!r}]: ir={av!r} ({type(av).__name__}) "
                f"compiled={ev!r} ({type(ev).__name__})"
            )

        assert a.value_flow.to_dict() == e.value_flow.to_dict(), (
            f"value_flow: ir={a.value_flow.to_dict()} compiled={e.value_flow.to_dict()}"
        )
        assert _node_hashes(a) == _node_hashes(e), (
            f"consumed event nodes: ir={_node_hashes(a)} compiled={_node_hashes(e)}"
        )


async def _compiled_and_ir_blocks(
    spec_path: Path,
    registries_factory,
    name: str,
    target_btype: str,
    trace,
    prereqs=(),
) -> tuple[list[Block], list[Block]]:
    """Compile `spec_path`, run the `name` matcher (compiled -> expected), emit
    the IR artifact, then build + run the same matcher through the IR engine
    (-> actual). Skips at the IR boundary if the engine package is absent, after
    the runnable-now half (compile + run + emit + render) has executed.
    """
    # --- compiled path (reference) ---
    compiled = compile_file(spec_path, registries_factory())
    compiled_by_name = {cm.name: cm.matcher for cm in compiled}
    assert name in compiled_by_name, f"{name!r} not compiled from {spec_path.name}"
    expected = [
        b for b in await _run_matcher(compiled_by_name[name], trace, prereqs)
        if b.btype == target_btype
    ]

    # --- emit IR + JSON round trip (proves the emit plumbing; runnable now) ---
    artifact = emit_ir(compiled, [spec_path.name])
    render_json(artifact)  # must not raise

    if not _HAVE_IR:
        pytest.skip(_SKIP_REASON)

    # --- IR-engine path (round-tripped) ---
    art = _mch_ir.load_artifact(artifact)
    ir_matchers = _mch_ir.build_matchers(art, registries_factory())
    ir_by_name = {m.name: m for m in ir_matchers}
    assert name in ir_by_name, f"{name!r} not built by the IR engine from {spec_path.name}"
    ir_matcher = ir_by_name[name]
    assert target_btype in list(ir_matcher.produces), (
        f"IR matcher {name!r} produces {list(ir_matcher.produces)}, "
        f"expected {target_btype!r}"
    )
    actual = [
        b for b in await _run_matcher(ir_matcher, trace, prereqs)
        if b.btype == target_btype
    ]
    return expected, actual


# ---------------------------------------------------------------------------
# Tier 1 — fully declarative jettons (specs_decl/jettons.mch): build statements
# + `out {...}`, no registered builder. Same 6 fixtures as test_jettons_decl.py.
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


@pytest.mark.asyncio
@pytest.mark.parametrize("name,filename", JETTON_CASES, ids=_JETTON_IDS)
async def test_tier1_declarative_jettons_roundtrip(traces_dir, name, filename):
    trace = _load_trace(traces_dir, filename)
    expected, actual = await _compiled_and_ir_blocks(
        JETTONS_DECL_MCH, jetton_decl_registries, name, name, trace,
    )
    _assert_roundtrip_parity(expected, actual, name)


# ---------------------------------------------------------------------------
# Tier 2 — dedust hybrid (specs_decl/dedust_swap_hybrid.mch): declarative build
# + registered host fns. Same 4 fixtures as test_dedust_hybrid.py. Legacy
# JettonTransfer + PTon prerequisites prepare the tree on both sides (see module
# docstring). The last fixture rejects on both paths (pool data absent).
# ---------------------------------------------------------------------------
DEDUST_CASES = [
    ("C7bHOfSrBJPLHXMZkVnIQladzgYZHIlKOiJRvbxRRuI=.lz4", False),
    ("c8jOrHqyuJe1hMGx4fm1BTGKXgBQ5O35rbExCkhmRG8=.lz4", False),
    ("cq7UujFWNfSnpHdRKseD41yHqj4Yx3SCWsnkw8IRPoU=.lz4", False),
    ("7bcODUs7IVJBjdApVMCJyUKoztjS_k4msfHyYOwt3qk=.lz4", True),
]
_DEDUST_IDS = ["multihop", "multihop-2", "single_hop_ton_in", "reject-missing-pool"]


@pytest.mark.asyncio
@pytest.mark.parametrize("filename,expect_empty", DEDUST_CASES, ids=_DEDUST_IDS)
async def test_tier2_dedust_hybrid_roundtrip(traces_dir, filename, expect_empty):
    trace = _load_trace(traces_dir, filename)
    prereqs = [JettonTransferBlockMatcher(), PTonTransferMatcher()]
    expected, actual = await _compiled_and_ir_blocks(
        DEDUST_HYBRID_MCH, dedust_hybrid_registries, "dedust_swap_hybrid",
        "jetton_swap", trace, prereqs=prereqs,
    )
    _assert_roundtrip_parity(expected, actual, "jetton_swap", allow_empty=expect_empty)
    if expect_empty:
        # Reject case: compiled and IR must BOTH produce nothing (spec §10.7).
        assert expected == [] and actual == []


# ---------------------------------------------------------------------------
# Tier 3 — builder-based jettons (specs/jettons.mch): `build build_*` directives
# delegating to the shared imperative cores. Same 6 fixtures as
# test_jettons_family.py, real builder registries.
# ---------------------------------------------------------------------------
@pytest.mark.asyncio
@pytest.mark.parametrize("name,filename", JETTON_CASES, ids=_JETTON_IDS)
async def test_tier3_builder_jettons_roundtrip(traces_dir, name, filename):
    trace = _load_trace(traces_dir, filename)
    expected, actual = await _compiled_and_ir_blocks(
        JETTONS_BUILDER_MCH, jetton_registries, name, name, trace,
    )
    _assert_roundtrip_parity(expected, actual, name)


# ---------------------------------------------------------------------------
# Tier 4 — negative controls. All gated on the IR engine being present; the
# exact error surface is the teammate's, so these are tolerant (any exception)
# and the unbound-name one is xfail until the surface is pinned.
# ---------------------------------------------------------------------------
@pytest.mark.skipif(not _HAVE_IR, reason=_SKIP_REASON)
def test_neg_version_mismatch_rejected():
    """load_artifact is version-checked (schema: mch_ir_version == '0.1-poc')."""
    compiled = compile_file(JETTONS_DECL_MCH, jetton_decl_registries())
    artifact = emit_ir(compiled, [JETTONS_DECL_MCH.name])
    artifact["mch_ir_version"] = "9.9-not-a-real-version"
    with pytest.raises(Exception):
        _mch_ir.load_artifact(artifact)


@pytest.mark.skipif(not _HAVE_IR, reason=_SKIP_REASON)
def test_neg_unknown_predicate_rejected():
    """A node referencing a predicate name absent from the registries must fail
    to build. (The Go POC stubs unknown predicates + warns; the Python engine is
    expected to hard-fail per docs/mch-ir-poc.md 'Predicates'. If the engine
    instead adopts the permissive stub behavior, relax this to a warning check.)
    """
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
    must surface a clear runtime error. The observed surface is a rejection (the
    EvalError is caught and the match produces no block, spec §10.7); this stays
    green either way — a raised error at load/build/run OR a clean rejection ([])
    both count, so the test is robust if the engine later tightens this to a hard
    build error.
    """
    compiled = compile_file(JETTONS_DECL_MCH, jetton_decl_registries())
    artifact = emit_ir(compiled, [JETTONS_DECL_MCH.name])
    transfer = next(m for m in artifact["matchers"] if m["name"] == "jetton_transfer")
    # Rewrite the `sender` out-field to reference an unbound name.
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
        # A raised error at load/build/run is an acceptable "clear runtime error".
        return
    # Otherwise the unbound name must have rejected the match (no block produced).
    assert produced == [], (
        "unbound name in build program neither raised nor rejected the match"
    )
