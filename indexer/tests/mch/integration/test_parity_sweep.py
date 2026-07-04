"""Parity sweep: EVERY compiled `.mch` matcher vs its IR-engine round trip, over
EVERY corpus fixture that could fire it.

This generalizes test_ir_engine_parity.py (a hand-picked 19-case round trip) to
the whole spec set × the whole tests/traces corpus. For one (matcher, fixture)
pair two matcher graphs are built from the SAME spec and run over independently
constructed block trees:

    compiled  =  compile_file(spec, registries)                 (frontend)
    ir        =  compile_file -> emit_ir -> load_artifact
                   -> build_matchers                            (IR engine)

Every produced block is compared at the same bar as the existing parity tests:
btype, deep per-key `data` equality (value + exact type), `failed`/`broken`
flags, `value_flow.to_dict()`, and the consumed event-node multiset (sorted tx
hashes — order-independent, never compared in list order). A divergence names
the matcher, the fixture, and the exact differing field path.

Fixture selection (fast prefilter): every matcher in the current spec set anchors
on an `opcode_set` (verified at collection). A matcher can only fire on a trace
that carries one of its anchor opcodes as a raw message opcode, so a pair is
generated only when the anchor opcode set intersects the trace's message opcodes
(normalized `& 0xFFFFFFFF` on both sides — DB opcodes may be signed int32). This
is a sound over-approximation: it may keep a pair that ultimately doesn't fire
(both paths then produce nothing — still a valid "reject identically" check) but
never drops a pair that would fire. A matcher whose anchor opcode is absent from
the entire corpus (e.g. DNS) yields zero pairs and is surfaced as an EXPLICIT
skip, never silently dropped.

Registries come straight from indexer.events.mch.builders (the same factories the
per-spec integration tests use); no existing file is modified and no shared
helper extraction was needed. Interface data comes from each trace's own bundle
(TestInterfaceRepository), mirroring the `trace_data` fixture. No live DB, no
network, no new trace recording.

The harness depends ONLY on the IR engine's public API (`load_artifact`,
`build_matchers`) — never on engine internals — so it survives the concurrent
engine.py rewrite. If the engine package is not importable, the whole sweep
skips with a reason via `test_ir_engine_available`.
"""
from __future__ import annotations

import dataclasses
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import pytest

from indexer.events import context
from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.jettons import (
    JettonBurnBlockMatcher,
    JettonMintBlockMatcher,
    JettonTransferBlockMatcher,
    PTonTransferMatcher,
)
from indexer.events.blocks.nft import NftMintBlockMatcher
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
import indexer.events.mch as mch
from indexer.events.mch import compile_file
from indexer.events.mch.builders import (
    coffee_registries,
    dedust_hybrid_registries,
    dedust_registries,
    dedust_withdraw_registries,
    dns_registries,
    jetton_decl_registries,
    jetton_registries,
    stonfi_v2_registries,
)
# Port-wave factories live in their own modules (builders/__init__.py is not
# extended per-port; per-spec integration tests import the same way).
from indexer.events.mch.builders.coffee import coffee_liquidity_registries
from indexer.events.mch.builders.dedust import dedust_deposit_registries
from indexer.events.mch.builders.ethena import ethena_registries
from indexer.events.mch.builders.evaa import evaa_registries
from indexer.events.mch.builders.jvault import jvault_registries
from indexer.events.mch.builders.layerzero import layerzero_receive_registries
from indexer.events.mch.builders.nominator import nominator_registries
from indexer.events.mch.builders.stonfi_liquidity import stonfi_liquidity_registries
from indexer.events.mch.builders.telegram_nft import telegram_nft_registries
from indexer.events.mch.builders.tgbtc import tgbtc_registries
from indexer.events.mch.builders.tonco import tonco_liquidity_registries
from indexer.events.mch.builders.tonco_swap import tonco_swap_registries
from indexer.events.mch.builders.tonstakers import tonstakers_registries
from indexer.events.mch.builders.vesting import vesting_registries
from indexer.events.mch.ir_emit import emit_ir
import indexer.events.mch.specs as specs_pkg
from tests.utils.repository import TestInterfaceRepository
from tests.utils.trace_deserializer import load_trace_from_file

# --- IR engine gate (public API only; may be mid-rewrite) -------------------
try:
    from indexer.events import mch_ir as _mch_ir

    _HAVE_IR = hasattr(_mch_ir, "load_artifact") and hasattr(_mch_ir, "build_matchers")
    _IR_IMPORT_ERR: BaseException | None = None
except Exception as ex:  # pragma: no cover - import error path
    _mch_ir = None
    _HAVE_IR = False
    _IR_IMPORT_ERR = ex

_SPECS_DIR = Path(specs_pkg.__file__).parent
_DECL_DIR = Path(mch.__file__).parent / "specs_decl"
_TRACES_DIR = Path(__file__).resolve().parents[2] / "traces"


# ---------------------------------------------------------------------------
# Spec table: (short key, spec path, registries factory, prerequisite matcher
# classes replayed to prepare the tree before the DSL matcher runs). Prereqs
# are LEGACY tree preparation identical on both paths (there is no pton `.mch`
# to round-trip), so they stay neutral — see test_ir_engine_parity's docstring.
# ---------------------------------------------------------------------------
_PREREQ_SWAP = (JettonTransferBlockMatcher, PTonTransferMatcher)

_SPEC_TABLE: list[tuple[str, Path, Callable, tuple]] = [
    ("specs/jettons.mch", _SPECS_DIR / "jettons.mch", jetton_registries, ()),
    ("specs/jetton_transfer.mch", _SPECS_DIR / "jetton_transfer.mch", jetton_registries, ()),
    ("specs/coffee_swap.mch", _SPECS_DIR / "coffee_swap.mch", coffee_registries, _PREREQ_SWAP),
    ("specs/dedust_swap.mch", _SPECS_DIR / "dedust_swap.mch", dedust_registries, _PREREQ_SWAP),
    ("specs/dedust_withdraw.mch", _SPECS_DIR / "dedust_withdraw.mch", dedust_withdraw_registries, _PREREQ_SWAP),
    ("specs/dns.mch", _SPECS_DIR / "dns.mch", dns_registries, ()),
    ("specs/stonfi_v2_swap.mch", _SPECS_DIR / "stonfi_v2_swap.mch", stonfi_v2_registries, _PREREQ_SWAP),
    ("specs_decl/jettons.mch", _DECL_DIR / "jettons.mch", jetton_decl_registries, ()),
    ("specs_decl/dedust_swap_hybrid.mch", _DECL_DIR / "dedust_swap_hybrid.mch", dedust_hybrid_registries, _PREREQ_SWAP),
    # Port wave (2026-07-04). Prereqs mirror each spec's own integration test.
    ("specs/evaa.mch", _SPECS_DIR / "evaa.mch", evaa_registries, (JettonTransferBlockMatcher,)),
    ("specs/layerzero.mch", _SPECS_DIR / "layerzero.mch", layerzero_receive_registries, ()),
    ("specs/tonstakers.mch", _SPECS_DIR / "tonstakers.mch", tonstakers_registries, (NftMintBlockMatcher,)),
    ("specs/ethena.mch", _SPECS_DIR / "ethena.mch", ethena_registries, (JettonTransferBlockMatcher, JettonMintBlockMatcher)),
    ("specs/vesting.mch", _SPECS_DIR / "vesting.mch", vesting_registries, ()),
    ("specs/dedust_deposit.mch", _SPECS_DIR / "dedust_deposit.mch", dedust_deposit_registries, _PREREQ_SWAP),
    ("specs/tonco_liquidity.mch", _SPECS_DIR / "tonco_liquidity.mch", tonco_liquidity_registries, (NftMintBlockMatcher,) + _PREREQ_SWAP),
    ("specs/coffee_liquidity.mch", _SPECS_DIR / "coffee_liquidity.mch", coffee_liquidity_registries, ()),
    # R02 remake — jvault family (stake btype-anchored, the other three op-anchored).
    ("specs/jvault.mch", _SPECS_DIR / "jvault.mch", jvault_registries, _PREREQ_SWAP),
    # Remake wave 1 (2026-07-04): R01/R03/R04/R05.
    ("specs/telegram_nft.mch", _SPECS_DIR / "telegram_nft.mch", telegram_nft_registries, ()),
    ("specs/tgbtc.mch", _SPECS_DIR / "tgbtc.mch", tgbtc_registries, (JettonMintBlockMatcher,)),
    # stonfi_v2_withdraw is btype-anchored (jetton_burn) — run-on-all row, rejects everywhere off-protocol.
    ("specs/stonfi_v2_withdraw.mch", _SPECS_DIR / "stonfi_v2_withdraw.mch", stonfi_liquidity_registries,
     (JettonTransferBlockMatcher, PTonTransferMatcher, JettonBurnBlockMatcher)),
    # nominator withdraw is btype-anchored (ton_transfer) — run-on-all row.
    ("specs/nominator.mch", _SPECS_DIR / "nominator.mch", nominator_registries, ()),
    # Remake wave 2 (2026-07-04): R11/T28 cyclic recursion.
    ("specs/tonco_swap.mch", _SPECS_DIR / "tonco_swap.mch", tonco_swap_registries, _PREREQ_SWAP),
]


@dataclass(frozen=True)
class _MatcherEntry:
    spec_key: str
    spec_path: Path
    factory: Callable
    prereqs: tuple
    name: str
    # None only for a (non-existent, defensively handled) non-opcode anchor.
    anchor_opcodes: frozenset[int] | None

    def __str__(self) -> str:  # nice repr in ids/messages
        return f"{self.spec_key}::{self.name}"


# ---------------------------------------------------------------------------
# Collection-time discovery: compile + emit each spec once to learn its matcher
# names and anchor opcodes. A spec that fails offline is recorded (surfaced by
# test_broken_specs), not crashed on.
# ---------------------------------------------------------------------------
def _discover_matchers() -> tuple[list[_MatcherEntry], list[tuple[str, str]]]:
    entries: list[_MatcherEntry] = []
    broken: list[tuple[str, str]] = []
    for key, path, factory, prereqs in _SPEC_TABLE:
        try:
            compiled = compile_file(path, factory())
            artifact = emit_ir(compiled, [path.name])
        except Exception as ex:  # pragma: no cover - defensive
            broken.append((key, f"{type(ex).__name__}: {ex}"))
            continue
        for m in artifact["matchers"]:
            anchor = m["anchor"]
            if anchor["kind"] == "opcode_set":
                ops: frozenset[int] | None = frozenset(v & 0xFFFFFFFF for v in anchor["values"])
            else:  # no matcher currently uses a btype anchor; run on all fixtures
                ops = None
            entries.append(_MatcherEntry(key, path, factory, prereqs, m["name"], ops))
    return entries, broken


def _scan_traces() -> dict[str, tuple]:
    """filename -> (trace, interfaces, opcode_frozenset). Loaded once; the trace
    is reused read-only across pairs (each run builds a fresh Block tree)."""
    cache: dict[str, tuple] = {}
    for f in sorted(_TRACES_DIR.glob("*.lz4")):
        trace, interfaces = load_trace_from_file(f)
        ops = frozenset(
            (m.opcode & 0xFFFFFFFF)
            for tx in trace.transactions
            for m in tx.messages
            if m.opcode is not None
        )
        cache[f.name] = (trace, interfaces, ops)
    return cache


_MATCHERS, _BROKEN_SPECS = _discover_matchers()
# Only pay the corpus-load cost when the sweep can actually run.
_TRACE_CACHE: dict[str, tuple] = _scan_traces() if _HAVE_IR else {}


def _build_pairs() -> tuple[list, list[_MatcherEntry]]:
    """Return (fired-candidate pairs, matchers with no candidate fixture)."""
    pairs: list = []
    no_fixture: list[_MatcherEntry] = []
    for entry in _MATCHERS:
        hits = [
            fname
            for fname, (_t, _i, ops) in _TRACE_CACHE.items()
            if entry.anchor_opcodes is None or (entry.anchor_opcodes & ops)
        ]
        if hits:
            for fname in hits:
                pairs.append((entry, fname))
        else:
            no_fixture.append(entry)
    return pairs, no_fixture


_PAIRS, _NO_FIXTURE = _build_pairs()
_PAIR_IDS = [f"{e.spec_key}::{e.name}::{fn[:-4]}" for e, fn in _PAIRS]


# ---------------------------------------------------------------------------
# Harness (copied verbatim from the existing parity tests so the same BFS +
# tree-construction prelude runs against both matcher graphs).
# ---------------------------------------------------------------------------
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


def _values_equal(a, b) -> bool:
    if a is None and b is None:
        return True
    if type(a) is not type(b):
        return False
    if isinstance(a, list):
        # List ORDER is not a parity property here: the compiled and IR trees
        # are built independently, and legacy compact_connections() rebuilds
        # next_blocks via list(set(...)) (T21), so order-bearing collections
        # (tonco `excesses`, dedust `vault_excesses`) can flip between the two
        # trees even legacy-vs-legacy. Compare as multisets, like the per-port
        # integration tests do.
        if len(a) != len(b):
            return False
        if a == b:
            return True
        return sorted(map(repr, a)) == sorted(map(repr, b))
    return bool(a == b)


def _node_hashes(block: Block) -> list[str]:
    return sorted(n.message.tx_hash for n in block.event_nodes)


def _sort_key(block: Block):
    return (block.min_lt, tuple(_node_hashes(block)))


def _data_items(data) -> dict:
    """`block.data` as a comparable mapping. Legacy-style blocks carry dicts;
    the port-wave blocks carry dataclasses (VestingSendMessageData,
    EvaaWithdrawData, ...). vars() is shallow on purpose — nested values go
    through _values_equal like dict values do."""
    if data is None:
        return {}
    if isinstance(data, dict):
        return data
    if dataclasses.is_dataclass(data):
        return vars(data)
    return {"<data>": data}


def _diff_blocks(compiled_blocks: list[Block], ir_blocks: list[Block]) -> list[str]:
    """Field-level divergences between the compiled blocks and the IR blocks.
    Empty list == identical. Each line is an actionable field path."""
    divs: list[str] = []
    if len(compiled_blocks) != len(ir_blocks):
        divs.append(
            f"  produced-block count: compiled={len(compiled_blocks)} ir={len(ir_blocks)}"
        )
    cs = sorted(compiled_blocks, key=_sort_key)
    irs = sorted(ir_blocks, key=_sort_key)
    for i, (c, ir) in enumerate(zip(cs, irs)):
        p = f"  block#{i}"
        if c.btype != ir.btype:
            divs.append(f"{p} btype: compiled={c.btype!r} ir={ir.btype!r}")
        if c.failed != ir.failed:
            divs.append(f"{p} failed: compiled={c.failed} ir={ir.failed}")
        if c.broken != ir.broken:
            divs.append(f"{p} broken: compiled={c.broken} ir={ir.broken}")

        if type(c.data) is not type(ir.data):
            divs.append(
                f"{p} data container: compiled={type(c.data).__name__} "
                f"ir={type(ir.data).__name__}"
            )
        cd, id_ = _data_items(c.data), _data_items(ir.data)
        ck, ik = set(cd.keys()), set(id_.keys())
        if ck != ik:
            divs.append(
                f"{p} data keys: compiled-only={sorted(ck - ik)} ir-only={sorted(ik - ck)}"
            )
        for key in sorted(ck & ik):
            cv, iv = cd[key], id_[key]
            if not _values_equal(cv, iv):
                divs.append(
                    f"{p} data[{key!r}]: compiled={cv!r} ({type(cv).__name__}) "
                    f"ir={iv!r} ({type(iv).__name__})"
                )

        cvf, ivf = c.value_flow.to_dict(), ir.value_flow.to_dict()
        if cvf != ivf:
            divs.append(f"{p} value_flow: compiled={cvf} ir={ivf}")

        ch, ih = _node_hashes(c), _node_hashes(ir)
        if ch != ih:
            divs.append(f"{p} consumed nodes: compiled={ch} ir={ih}")
    return divs


def _build_both(entry: _MatcherEntry):
    """Compile the spec once; return (compiled_matcher, ir_matcher) by name."""
    compiled = compile_file(entry.spec_path, entry.factory())
    compiled_by_name = {cm.name: cm.matcher for cm in compiled}
    assert entry.name in compiled_by_name, (
        f"{entry.name!r} not compiled from {entry.spec_path.name}"
    )
    artifact = emit_ir(compiled, [entry.spec_path.name])
    art = _mch_ir.load_artifact(artifact)
    ir_by_name = {m.name: m for m in _mch_ir.build_matchers(art, entry.factory())}
    assert entry.name in ir_by_name, (
        f"{entry.name!r} not built by the IR engine from {entry.spec_path.name}"
    )
    return compiled_by_name[entry.name], ir_by_name[entry.name]


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
def test_ir_engine_available():
    """Whole-sweep gate. Skips (with the import error) when the engine package
    is mid-rewrite, so every parametrized pair below is not silently green."""
    if not _HAVE_IR:
        pytest.skip(f"indexer.events.mch_ir not importable ({_IR_IMPORT_ERR}); sweep cannot run")


def test_no_broken_specs():
    """Every spec in the table must compile + emit its IR artifact offline (no
    registry/predicate/builder gap). Fails loudly, listing any that don't."""
    assert not _BROKEN_SPECS, "specs failed to compile/emit offline:\n" + "\n".join(
        f"  {k}: {r}" for k, r in _BROKEN_SPECS
    )


@pytest.mark.skipif(not _NO_FIXTURE, reason="every matcher has ≥1 candidate fixture")
@pytest.mark.parametrize(
    "entry",
    _NO_FIXTURE or [None],
    ids=[str(e) for e in _NO_FIXTURE] or ["none"],
)
def test_matcher_has_no_corpus_fixture(entry: _MatcherEntry):
    """A matcher whose anchor opcode(s) never appear in the 147-trace corpus:
    there is no round-trip pair to compare. Surfaced as an explicit skip (its
    behavior is covered by a synthetic-trace test, e.g. test_dns.py)."""
    opcodes = sorted(hex(o) for o in (entry.anchor_opcodes or ()))
    pytest.skip(
        f"{entry.spec_key}::{entry.name}: anchor opcode(s) {opcodes} absent from all "
        f"{len(_TRACE_CACHE)} corpus fixtures; no compiled-vs-IR pair to compare"
    )


@pytest.mark.asyncio
@pytest.mark.parametrize("entry,filename", _PAIRS, ids=_PAIR_IDS)
async def test_sweep_parity(entry: _MatcherEntry, filename: str):
    if not _HAVE_IR:
        pytest.skip(f"indexer.events.mch_ir not importable ({_IR_IMPORT_ERR})")

    trace, interfaces, _ops = _TRACE_CACHE[filename]
    context.interface_repository.set(TestInterfaceRepository(interfaces))

    compiled_matcher, ir_matcher = _build_both(entry)

    compiled_blocks = await _run_matcher(
        compiled_matcher, trace, [cls() for cls in entry.prereqs]
    )
    ir_blocks = await _run_matcher(ir_matcher, trace, [cls() for cls in entry.prereqs])

    divs = _diff_blocks(compiled_blocks, ir_blocks)
    assert not divs, "\n".join(
        [f"DIVERGENCE {entry.spec_key}::{entry.name} on {filename} "
         f"(compiled={len(compiled_blocks)} ir={len(ir_blocks)} blocks):"]
        + divs
    )
