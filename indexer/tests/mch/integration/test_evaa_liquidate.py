"""Legacy-vs-IR parity and synthetic outcome gates for EVAA liquidation."""
from pathlib import Path
from types import SimpleNamespace

import pytest

from indexer.events import context, mch_ir
from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block
from indexer.events.blocks.evaa import EvaaLiquidateBlockMatcher
from indexer.events.blocks.jettons import JettonTransferBlockMatcher
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_file
from indexer.events.mch.builders import evaa as evaa_builder
from indexer.events.mch.builders.evaa import evaa_registries
from indexer.events.mch.ir_emit import emit_ir
import indexer.events.mch.specs as specs_pkg
from tests.utils.repository import TestInterfaceRepository
from tests.utils.trace_deserializer import load_trace_from_file

EVAA_MCH = Path(specs_pkg.__file__).parent / "evaa.mch"
OUTPUT_KEYS = ("liquidator", "borrower", "borrower_contract", "collateral_asset_id",
               "collateral_amount", "is_success", "fail_reason", "debt_amount",
               "debt_asset_id")


async def _run(matcher, trace):
    root = Block("root", [])
    root.connect(init_block(to_tree(trace.transactions)))
    jettons = JettonTransferBlockMatcher()
    for block in root.bfs_iter():
        if block.parent is None:
            await jettons.try_build(block)
    out = []
    for block in root.bfs_iter():
        if block.btype != "root" and block.parent is None:
            built = await matcher.try_build(block)
            if built:
                out.extend(b for b in built if b.btype == "evaa_liquidate")
    return out


def _get(data, key):
    return data[key] if isinstance(data, dict) else getattr(data, key)


@pytest.mark.asyncio
@pytest.mark.parametrize(("fixture", "debt_amount", "asset_id"), [
    ("ADMo80jSlcAWGOudmITAHQEX13l9VKWYF5w80Fd2pPs=.lz4", 3774186907836329050,
     0x117C66CB95754D4),
    ("f4J1gD7fmSBUMJy47G3qmk7CvXa1O8Ofr7Ehw-Ttd2Y=.lz4", 12142081103633125532,
     0x1C1163E125CE481),
    ("Fj6hSkHT6F-PyFB1F3-hq2alVtEYB0BkAzPQCGUp3BI=.lz4", 12142081103633125532,
     0x1C1163E125CE481),
])
async def test_evaa_liquidate_fixture_parity(traces_dir, fixture, debt_amount, asset_id):
    trace, interfaces = load_trace_from_file(traces_dir / fixture)
    context.interface_repository.set(TestInterfaceRepository(interfaces))
    legacy = await _run(EvaaLiquidateBlockMatcher(), trace)
    assert len(legacy) == 1

    compiled = compile_file(EVAA_MCH, evaa_registries())
    artifact = emit_ir(compiled, [EVAA_MCH.name])
    matchers = {m.name: m for m in mch_ir.build_matchers(
        mch_ir.load_artifact(artifact), evaa_registries())}
    actual = await _run(matchers["evaa_liquidate"], trace)
    assert len(actual) == 1
    actual, legacy = actual[0], legacy[0]
    assert actual.failed == legacy.failed and actual.broken == legacy.broken
    for key in OUTPUT_KEYS:
        assert _get(actual.data, key) == _get(legacy.data, key), key
    assert actual.value_flow.to_dict() == legacy.value_flow.to_dict()
    assert sorted(n.message.tx_hash for n in actual.event_nodes) == sorted(
        n.message.tx_hash for n in legacy.event_nodes)
    assert set(actual.data) == set(OUTPUT_KEYS)
    assert actual.data["fail_reason"] == "unknown"
    assert actual.data["debt_amount"] == debt_amount
    assert actual.data["collateral_asset_id"] == asset_id


def _call(opcode, *, bounced=False):
    block = object.__new__(CallContractBlock)
    Block.__init__(block, "call_contract", [])
    block.opcode = opcode
    block.get_body = lambda: object()
    block.get_message = lambda: SimpleNamespace(
        source="0:" + "1" * 64, destination="0:" + "2" * 64, bounced=bounced)
    return block


@pytest.mark.asyncio
async def test_evaa_liquidate_synthetic_outcomes(monkeypatch):
    master = SimpleNamespace(
        borrower_address=None, collateral_asset_id=0xAA, liquidate_incoming_amount=900)
    monkeypatch.setattr(evaa_builder, "EvaaLiquidateMaster", lambda _: master)
    monkeypatch.setattr(evaa_builder, "_opcode_after_user_header",
                        lambda b, op: getattr(b, "header_opcode", None) == op)

    immediate = await evaa_builder._evaa_liquidate_data(
        [_call(0x3), _call(0xFFFFFFFF, bounced=True)])
    assert immediate["borrower_contract"] is None
    assert immediate["fail_reason"] == "immediate_rejection"

    anchor, user, unsatisfied = _call(0x3), _call(0x31), _call(0x31F)
    user.header_opcode = 0x31
    broken_unsatisfied = lambda _: (_ for _ in ()).throw(ValueError("bad residual slice"))
    broken_unsatisfied.opcode = evaa_builder.EvaaLiquidateUnsatisfied.opcode
    monkeypatch.setattr(evaa_builder, "EvaaLiquidateUnsatisfied", broken_unsatisfied)
    fallback = await evaa_builder._evaa_liquidate_data([anchor, user, unsatisfied])
    assert fallback["fail_reason"] == "liquidation_error"

    satisfied = _call(0x311)
    satisfied.next_blocks = [_call(0x311F)]
    failed = await evaa_builder._evaa_liquidate_data([anchor, user, satisfied])
    assert failed["fail_reason"] == "master_not_enough_liquidity"

    success = _call(0x311A)
    success.header_opcode = 0x311A
    satisfied.next_blocks = [success]
    parsed = SimpleNamespace(owner_address=None, collateral_asset_id=0xBB,
                             collateral_reward=77, liquidatable_amount=88,
                             transferred_asset_id=0xCC)
    parsed_satisfied = lambda _: parsed
    parsed_satisfied.opcode = evaa_builder.EvaaLiquidateSatisfied.opcode
    monkeypatch.setattr(evaa_builder, "EvaaLiquidateSatisfied", parsed_satisfied)
    succeeded = await evaa_builder._evaa_liquidate_data([anchor, user, satisfied])
    assert succeeded["is_success"] is True
    assert (succeeded["collateral_asset_id"], succeeded["collateral_amount"],
            succeeded["debt_amount"]) == (0xBB, 77, 88)
    assert succeeded["debt_asset_id"] == 0xCC
