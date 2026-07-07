"""R02 remake — three-way parity for the jvault family (specs/jvault.mch):
legacy JVault*BlockMatcher == compiled mch path == v2 IR engine, on every
jvault.yaml fixture.

Same assertion depth as test_evaa_withdraw.py: btype, per-field `data`
equality (the four JVault* dataclasses), failed/broken flags, value_flow, and
the consumed event-node multiset. Block topology (children_blocks/next_blocks)
is not asserted separately: every build path ends in
`merge_blocks(consumed_set)`, so identical consumed sets deterministically
produce identical topology — the remake gate's topology clause is discharged by
the consumed-node-set equality below.

The referral recursion is exercised by four shapes across the corpus: absent
(stake_simple / unstake_failed), a single RequestUpdateReferrer + excess
(stake_partial / unstake_request), a linear 3× RequestUpdateReferrer +
UpdateReferrer chain (stake_full), a branching/multi referral set (unstake),
and a failed referral that bounces (unstake_request_failed_referral). The
legacy nested RecursiveMatcher flattens to the single `$jvault_referrals`
recursion with identical consumption on all of them.

Prerequisites: JettonTransferBlockMatcher (+ PTonTransferMatcher) prepare the
`jetton_transfer` blocks the stake anchor and the unstake/claim withdraw legs
depend on — same neutral tree preparation on both paths.
"""
from __future__ import annotations

from dataclasses import fields, is_dataclass
from pathlib import Path

import pytest

from indexer.events import context
from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.jettons import JettonTransferBlockMatcher, PTonTransferMatcher
from indexer.events.blocks.jvault import (
    JVaultClaimBlockMatcher,
    JVaultStakeBlockMatcher,
    JVaultUnstakeBlockMatcher,
    JVaultUnstakeRequestBlockMatcher,
)
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_file
from indexer.events.mch.builders.jvault import jvault_registries
from indexer.events.mch.ir_emit import emit_ir, render_json
import indexer.events.mch.specs as specs_pkg
from tests.utils.repository import TestInterfaceRepository

# --- IR engine gate (same convention as test_evaa_withdraw) -----------------
try:
    from indexer.events import mch_ir as _mch_ir

    _HAVE_IR = hasattr(_mch_ir, "load_artifact") and hasattr(_mch_ir, "build_matchers")
except Exception:  # pragma: no cover - import error path
    _mch_ir = None
    _HAVE_IR = False

_SKIP_REASON = (
    "indexer.events.mch_ir not importable; the legacy-vs-compiled parity and "
    "the IR emit above still ran"
)

JVAULT_MCH = Path(specs_pkg.__file__).parent / "jvault.mch"

# (matcher name in the spec, legacy matcher class)
_MATCHER_FOR = {
    "jvault_stake": JVaultStakeBlockMatcher,
    "jvault_unstake": JVaultUnstakeBlockMatcher,
    "jvault_claim": JVaultClaimBlockMatcher,
    "jvault_unstake_request": JVaultUnstakeRequestBlockMatcher,
}


def _build_block_tree(trace) -> Block:
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matcher(matcher: BlockMatcher, trace) -> list:
    root = _build_block_tree(trace)
    for pre in (JettonTransferBlockMatcher(), PTonTransferMatcher()):
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


def _assert_field_parity(actual: list[Block], expected: list[Block], btype: str,
                         actual_tag: str, expected_tag: str) -> None:
    assert len(expected) > 0, f"{expected_tag} produced no {btype} blocks - bad fixture"
    assert len(actual) == len(expected), (
        f"count mismatch: {actual_tag}={len(actual)} {expected_tag}={len(expected)}"
    )
    for a, e in zip(sorted(actual, key=_sort_key), sorted(expected, key=_sort_key)):
        assert a.btype == e.btype == btype
        assert a.failed == e.failed, f"failed flag: {actual_tag}={a.failed} {expected_tag}={e.failed}"
        assert a.broken == e.broken, f"broken flag: {actual_tag}={a.broken} {expected_tag}={e.broken}"

        # specs/jvault.mch is now a declarative build, so the IR block's `data`
        # is the canonical dict (bare btype + map); the legacy block carries a
        # JVault* dataclass. Compare field-for-field over the dataclass schema
        # regardless of shape (wave-B btype+data acceptance, cf. test_evaa).
        assert is_dataclass(e.data)

        def _get(d, name):
            return d[name] if isinstance(d, dict) else getattr(d, name)

        for f in fields(e.data):
            av, ev = _get(a.data, f.name), _get(e.data, f.name)
            assert _values_equal(av, ev), (
                f"data.{f.name}: {actual_tag}={av!r} ({type(av).__name__}) "
                f"{expected_tag}={ev!r} ({type(ev).__name__})"
            )

        assert a.value_flow.to_dict() == e.value_flow.to_dict(), (
            f"value_flow: {actual_tag}={a.value_flow.to_dict()} "
            f"{expected_tag}={e.value_flow.to_dict()}"
        )
        assert _node_hashes(a) == _node_hashes(e), (
            f"consumed event nodes: {actual_tag}={_node_hashes(a)} "
            f"{expected_tag}={_node_hashes(e)}"
        )


async def _run_parity(trace, matcher_name: str) -> list[Block]:
    """legacy vs the v2 IR engine (the compiled path is retired — T29). Returns
    the legacy blocks for per-case semantic pins."""
    btype = matcher_name
    legacy_cls = _MATCHER_FOR[matcher_name]

    context.interface_repository.set(TestInterfaceRepository(_INTERFACES))
    hw = [b for b in await _run_matcher(legacy_cls(), trace) if b.btype == btype]

    # Compiling is a frontend-internal step ir_emit needs; no compiled matcher is
    # run as a runtime. IR emit + JSON render always run (a break here must error,
    # not skip).
    compiled = compile_file(JVAULT_MCH, jvault_registries())
    artifact = emit_ir(compiled, [JVAULT_MCH.name])
    render_json(artifact)
    if not _HAVE_IR:
        pytest.skip(_SKIP_REASON)

    art = _mch_ir.load_artifact(artifact)
    ir_by_name = {m.name: m for m in _mch_ir.build_matchers(art, jvault_registries())}
    context.interface_repository.set(TestInterfaceRepository(_INTERFACES))
    ir = [b for b in await _run_matcher(ir_by_name[matcher_name], trace) if b.btype == btype]
    _assert_field_parity(ir, hw, btype, "ir", "legacy")
    return hw


# Interfaces come from each trace's own bundle (set by the trace_data fixture
# indirectly); jvault's extract_jvault_assets reads get_extra_data /
# get_jetton_wallet off the interface repository, so we install the trace's
# bundle before each run. The trace_data fixture provides (trace, interfaces).
_INTERFACES: dict = {}


def _install(trace_data):
    global _INTERFACES
    trace, interfaces = trace_data
    _INTERFACES = interfaces
    return trace


@pytest.mark.asyncio
@pytest.mark.trace_file("02lXjywHnaBuI4CGq55IpcuxA0Q5QMOlYsoJwii4SNI=.lz4")
async def test_stake_partial_referral(trace_data):
    trace = _install(trace_data)
    hw = await _run_parity(trace, "jvault_stake")
    assert hw[0].failed is False


@pytest.mark.asyncio
@pytest.mark.trace_file("00vuJ5v4lkaAELRDh9hh7LyCwiLfpHUpiudpNOiHdnA=.lz4")
async def test_stake_simple(trace_data):
    trace = _install(trace_data)
    await _run_parity(trace, "jvault_stake")


@pytest.mark.asyncio
@pytest.mark.trace_file("3gHUMt8h1SyaO7pMTIWZ3d0adXpyHbvb7j-0RZQFjiU=.lz4")
async def test_stake_failed_cancellation(trace_data):
    trace = _install(trace_data)
    hw = await _run_parity(trace, "jvault_stake")
    assert hw[0].failed is True


@pytest.mark.asyncio
@pytest.mark.trace_file("-OII6CIrWH76IRnP252ExZ3hWGxs3hUjm1JGCFLF3cU=.lz4")
async def test_stake_full_referral_chain(trace_data):
    """Linear 3x RequestUpdateReferrer -> UpdateReferrer -> excess referral chain
    — the flattened single recursion consumes it identically to the legacy
    nested RecursiveMatcher."""
    trace = _install(trace_data)
    await _run_parity(trace, "jvault_stake")


@pytest.mark.asyncio
@pytest.mark.trace_file("62Oe2uSj1TW6uIN-hfzhSE8JpZUn5S5pZiWFIRhgldY=.lz4")
async def test_unstake_with_referrals(trace_data):
    """Branching referral set + an unstake_fee ton_transfer leg."""
    trace = _install(trace_data)
    hw = await _run_parity(trace, "jvault_unstake")
    assert hw[0].data.unstake_fee_taken is not None


@pytest.mark.asyncio
@pytest.mark.trace_file("mTVGq-A5PCaXAdu-Xuab0J-vtygXRJ1GwqZbW-aYN84=.lz4")
async def test_unstake_failed_early(trace_data):
    """No request_update reached the pool — the exit-code build path (anchor +
    bounce only)."""
    trace = _install(trace_data)
    hw = await _run_parity(trace, "jvault_unstake")
    assert hw[0].data.exit_code == 56


@pytest.mark.asyncio
@pytest.mark.trace_file("Lsy5I_gotIvt0EYwoPuqd-1s-IyYNR3Sc0XmM_lWKa8=.lz4")
async def test_claim(trace_data):
    trace = _install(trace_data)
    hw = await _run_parity(trace, "jvault_claim")
    assert len(hw[0].data.claimed_jettons) == 1


@pytest.mark.asyncio
@pytest.mark.trace_file("0uFDUicjsZVS8Ofe2OAZLO0FPks-g5LFuGhWYYYh2I0=.lz4")
async def test_unstake_request(trace_data):
    trace = _install(trace_data)
    await _run_parity(trace, "jvault_unstake_request")


@pytest.mark.asyncio
@pytest.mark.trace_file("4SLKuSOof3oAYQV1RKIMHgSdqKnTFqz4bn4wMlzZHFk=.lz4")
async def test_unstake_request_failed_referral(trace_data):
    """A referral that fails and bounces (op 0xffffffff) — the bounce is
    auto-consumed by the referral recursion's `op` node include_bounces."""
    trace = _install(trace_data)
    await _run_parity(trace, "jvault_unstake_request")


@pytest.mark.asyncio
@pytest.mark.trace_file("CvKIwyXweBMr92O5aPbqmPJJWXgklMpKmawq7HRB6d0=.lz4")
async def test_unstake_request_success_referral(trace_data):
    trace = _install(trace_data)
    await _run_parity(trace, "jvault_unstake_request")


@pytest.mark.asyncio
@pytest.mark.trace_file("k4j3bwj6FjSeLcdN8t4usf2Dq4qWpHkNDm2WH8_qeq4=.lz4")
async def test_unstake_request_failed(trace_data):
    trace = _install(trace_data)
    hw = await _run_parity(trace, "jvault_unstake_request")
    assert hw[0].failed is True
    assert hw[0].data.exit_code == 46
