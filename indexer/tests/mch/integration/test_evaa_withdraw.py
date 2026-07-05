"""Field-level parity between EvaaWithdrawBlockMatcher and specs/evaa.mch,
plus the IR round trip (compile -> emit_ir -> load_artifact -> build_matchers).

Same assertion depth as test_stonfi_v2_swap / test_ir_engine_parity: btype,
per-field `data` equality (EvaaWithdrawData dataclass), failed/broken flags,
value_flow, consumed event-node set. Count parity alone is not acceptance.
The header-skipping predicates are the real EvaaContractWithHeaderMatcher
logic (shared `load_user_header`), not stubs.

Fixtures (all six withdraw cases of tests/test_cases/evaa.yaml; no new
recording):
- n7wJbUq7_-Li9GUnwlje3eGe5u1mZQGNEXQlGV8L-cI=  TON withdrawal success —
    collateralized-success branch, `op EvaaWithdrawSuccess` ton_payout arm.
- OyAQgEdEKZcuU2-3dAkKpk_RFjTYJXXQYxCKoZSB7uA=  tsTON (jetton) withdrawal
    success — `btype jetton_transfer` payout arm.
- 0CHtYFyuAUpk75hWntPKTfF2qcVEfOXnt4BCzfzXD2w=  TON withdrawal, master-side
    fail — collateralized-fail branch (withdraw_fail -> no-funds excess).
- 02KO-3RSDiakxzyMbk2MxKMsVsAgtGM78n1aWdxqFm4=  jetton withdrawal rejected on
    the user contract: withdraw_locked_excess (0x21e6).
- 02N57kDv6SOZ0OaS-7TnLwTtvm5xt12EM7aaLOl5QKk=  jetton withdrawal rejected:
    withdraw_not_collateralized_excess (0x21e7).
- 0DBVOFKwlN16c0lzUq8T55Hzca_Yxuc52mDgXW-xBYU=  TON withdrawal rejected:
    withdraw_missing_prices_excess (0x21e8).

Corpus gap: no withdraw_execution_crashed (0x21ec) trace — that inner-XOR
branch is structurally identical to the other three fail-excess arms.

Prerequisite: JettonTransferBlockMatcher prepares `jetton_transfer` blocks
(the payout arm and event_processing both depend on it running first). EVAA
does not use pTON, so no PTonTransferMatcher is needed.
"""
from __future__ import annotations

from dataclasses import fields
from pathlib import Path

import pytest

from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.evaa import EvaaWithdrawBlockMatcher, EvaaWithdrawData
from indexer.events.blocks.jettons import JettonTransferBlockMatcher
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_file
from indexer.events.mch.builders.evaa import evaa_registries
from indexer.events.mch.ir_emit import emit_ir, render_json
import indexer.events.mch.specs as specs_pkg

# --- IR engine gate (same convention as test_ir_engine_parity) --------------
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

EVAA_MCH = Path(specs_pkg.__file__).parent / "evaa.mch"


def _build_block_tree(trace) -> Block:
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matcher(matcher: BlockMatcher, trace) -> list:
    root = _build_block_tree(trace)
    for pre in [JettonTransferBlockMatcher()]:
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
    return [b for b in matches if b.btype == "evaa_withdraw"]


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


def _assert_field_parity(actual: list[Block], expected: list[Block],
                         actual_tag: str, expected_tag: str) -> None:
    assert len(expected) > 0, f"{expected_tag} produced no evaa_withdraw blocks - bad fixture"
    assert len(actual) == len(expected), (
        f"count mismatch: {actual_tag}={len(actual)} {expected_tag}={len(expected)}"
    )
    for a, e in zip(sorted(actual, key=_sort_key), sorted(expected, key=_sort_key)):
        assert a.btype == e.btype == "evaa_withdraw"
        assert a.failed == e.failed, f"failed flag: {actual_tag}={a.failed} {expected_tag}={e.failed}"
        assert a.broken == e.broken, f"broken flag: {actual_tag}={a.broken} {expected_tag}={e.broken}"

        # specs/evaa.mch has a declarative build, so the IR block's `data` is the
        # canonical dict (btype + map); the legacy block carries EvaaWithdrawData.
        # Compare field-for-field over the dataclass schema regardless of shape
        # (wave-B btype+data acceptance).
        def _get(d, name):
            return d[name] if isinstance(d, dict) else getattr(d, name)
        for f in fields(EvaaWithdrawData):
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


async def _run_parity(trace) -> list[Block]:
    """legacy vs the v2 IR engine (the compiled path is retired — T29). Returns
    the legacy blocks for per-case semantic pins."""
    hw_blocks = await _run_matcher(EvaaWithdrawBlockMatcher(), trace)

    # Compiling the spec is a frontend-internal step ir_emit needs; no compiled
    # matcher is run as a runtime. IR emit + JSON render always run (a break here
    # must error, not skip).
    compiled = compile_file(EVAA_MCH, evaa_registries())
    artifact = emit_ir(compiled, [EVAA_MCH.name])
    render_json(artifact)
    if not _HAVE_IR:
        pytest.skip(_SKIP_REASON)

    art = _mch_ir.load_artifact(artifact)
    ir_by_name = {m.name: m for m in _mch_ir.build_matchers(art, evaa_registries())}
    ir_blocks = await _run_matcher(ir_by_name["evaa_withdraw"], trace)
    _assert_field_parity(ir_blocks, hw_blocks, "ir", "legacy")
    return hw_blocks


@pytest.mark.asyncio
@pytest.mark.trace_file("n7wJbUq7_-Li9GUnwlje3eGe5u1mZQGNEXQlGV8L-cI=.lz4")
async def test_evaa_withdraw_parity_ton_success(trace_data):
    trace, _ = trace_data
    hw = await _run_parity(trace)
    assert hw[0].data.is_success and hw[0].data.is_ton


@pytest.mark.asyncio
@pytest.mark.trace_file("OyAQgEdEKZcuU2-3dAkKpk_RFjTYJXXQYxCKoZSB7uA=.lz4")
async def test_evaa_withdraw_parity_jetton_success(trace_data):
    trace, _ = trace_data
    hw = await _run_parity(trace)
    assert hw[0].data.is_success and not hw[0].data.is_ton
    assert hw[0].data.recipient_jetton_wallet is not None, (
        "fixture no longer exercises the jetton payout arm"
    )


@pytest.mark.asyncio
@pytest.mark.trace_file("0CHtYFyuAUpk75hWntPKTfF2qcVEfOXnt4BCzfzXD2w=.lz4")
async def test_evaa_withdraw_parity_no_funds(trace_data):
    """Master-side failure: collateralized -> withdraw_fail -> no-funds excess."""
    trace, _ = trace_data
    hw = await _run_parity(trace)
    assert hw[0].data.fail_reason == "withdraw_no_funds_excess"


@pytest.mark.asyncio
@pytest.mark.trace_file("02KO-3RSDiakxzyMbk2MxKMsVsAgtGM78n1aWdxqFm4=.lz4")
async def test_evaa_withdraw_parity_locked(trace_data):
    trace, _ = trace_data
    hw = await _run_parity(trace)
    assert hw[0].data.fail_reason == "withdraw_locked_excess"


@pytest.mark.asyncio
@pytest.mark.trace_file("02N57kDv6SOZ0OaS-7TnLwTtvm5xt12EM7aaLOl5QKk=.lz4")
async def test_evaa_withdraw_parity_not_collateralized(trace_data):
    trace, _ = trace_data
    hw = await _run_parity(trace)
    assert hw[0].data.fail_reason == "withdraw_not_collateralized_excess"


@pytest.mark.asyncio
@pytest.mark.trace_file("0DBVOFKwlN16c0lzUq8T55Hzca_Yxuc52mDgXW-xBYU=.lz4")
async def test_evaa_withdraw_parity_missing_prices(trace_data):
    trace, _ = trace_data
    hw = await _run_parity(trace)
    assert hw[0].data.fail_reason == "withdraw_missing_prices_excess"
    assert hw[0].data.is_ton


# --- Same-head XOR capture-binding proof ------------------------------------
#
# The evaa_withdraw builder is consumed-set-driven precisely BECAUSE a naive
# same-head XOR walker binds the wrong branch (@coll_ok on FAIL traces, where the
# two `op EvaaWithdrawCollateralized` arms head-test identically). The v2 IR
# engine's _match_or does a full-subtree trial and binds the first FULLY matching
# branch, so on a fail trace it binds the FAIL-branch captures. We spy the Match
# object the IR engine hands the builder and assert the bindings directly (the
# shared consumed-set builder ignores captures, so the field parity above cannot
# observe this).

async def _binding_proof(trace, monkeypatch):
    """Run the IR evaa matcher over `trace`, returning the capture environment it
    bound (with @coll_ok / @coll_fail / @fail_excess) as a namespace.

    specs/evaa.mch is declarative — there is no build function to spy on. Instead
    we record the env the IR engine hands its BuildProgram (keyed by capture
    name), which is exactly where the exclusive-branch bindings land."""
    if not _HAVE_IR:
        pytest.skip(_SKIP_REASON)
    from types import SimpleNamespace

    from indexer.events.mch_ir import program as _progmod

    recorded: list = []
    real_run = _progmod.BuildProgram.run

    async def spy_run(self, env):
        recorded.append(dict(env))
        return await real_run(self, env)

    monkeypatch.setattr(_progmod.BuildProgram, "run", spy_run)

    reg_i = evaa_registries()
    compiled_i = compile_file(EVAA_MCH, reg_i)
    artifact = emit_ir(compiled_i, [EVAA_MCH.name])
    art = _mch_ir.load_artifact(artifact)
    ir_by_name = {m.name: m for m in _mch_ir.build_matchers(art, reg_i)}
    await _run_matcher(ir_by_name["evaa_withdraw"], trace)
    assert recorded, "IR path produced no evaa_withdraw match"
    return SimpleNamespace(**recorded[0])


@pytest.mark.asyncio
@pytest.mark.trace_file("0CHtYFyuAUpk75hWntPKTfF2qcVEfOXnt4BCzfzXD2w=.lz4")
async def test_evaa_no_funds_binds_coll_fail_not_coll_ok(trace_data, monkeypatch):
    """The core misbinding case: a real `op EvaaWithdrawCollateralized` (0x211)
    block exists and the FAIL branch (@coll_fail) is the one the trace matches.
    A naive first-head walker would bind @coll_ok; the IR engine binds
    @coll_fail."""
    trace, _ = trace_data
    ir_m = await _binding_proof(trace, monkeypatch)
    assert ir_m.coll_fail is not None, "IR engine must bind @coll_fail on the fail trace"
    assert ir_m.coll_ok is None, "IR engine must NOT bind @coll_ok (the misbinding)"


@pytest.mark.asyncio
@pytest.mark.trace_file("02KO-3RSDiakxzyMbk2MxKMsVsAgtGM78n1aWdxqFm4=.lz4")
async def test_evaa_locked_binds_fail_excess_only(trace_data, monkeypatch):
    """User-contract rejection: the winning arm is the third (fail_excess) XOR;
    neither collateralized capture binds."""
    trace, _ = trace_data
    ir_m = await _binding_proof(trace, monkeypatch)
    assert ir_m.fail_excess is not None
    assert ir_m.coll_ok is None
    assert ir_m.coll_fail is None


@pytest.mark.asyncio
@pytest.mark.trace_file("02N57kDv6SOZ0OaS-7TnLwTtvm5xt12EM7aaLOl5QKk=.lz4")
async def test_evaa_not_collateralized_binds_fail_excess_only(trace_data, monkeypatch):
    trace, _ = trace_data
    ir_m = await _binding_proof(trace, monkeypatch)
    assert ir_m.fail_excess is not None
    assert ir_m.coll_ok is None
    assert ir_m.coll_fail is None
