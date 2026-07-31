"""mch registration for the evaa_withdraw matcher (specs/evaa.mch).

specs/evaa.mch is DECLARATIVE (M4 §9): the runtime evaluates parse/reject/out
field-for-field with build_evaa_withdraw_core, so this module registers only the
opcodes, the produced block type, the message parsers (`parse`), and the four
host predicates the structural pattern needs. There is NO build function anymore
— the derive-parts consumed-set walker the compiled path required is gone (the
IR engine binds the exclusive-branch captures directly).

Host predicates (each a dual-implementation parity liability, spec §5.2 — the
named GAPS that keep this matcher from being predicate-free):

- evaa_user_withdraw_user / evaa_user_withdraw_success / evaa_user_withdraw_fail
  — EVAA user-contract messages carry a variable-length header before the 32-bit
  opcode, so a plain `op` test cannot identify them. `_opcode_after_user_header`
  replicates EvaaContractWithHeaderMatcher.test_self (blocks/evaa.py) byte for
  byte. This is body parsing, not an M4 inline field test.
- evaa_service_comment — legacy evaa_action_comment_matcher: a TonTransferBlock
  whose comment is one of the EVAA service comments (set membership over a block
  attribute; not an M4 inline field test either).

The legacy EvaaWithdrawBlockMatcher and build_evaa_withdraw_core stay the parity
reference.
"""
from __future__ import annotations

from pytoniq_core import Cell

from indexer.events.blocks.basic_blocks import CallContractBlock, TonTransferBlock
from indexer.events.blocks.core import Block
from indexer.events.blocks.evaa import (
    EVAA_ACTION_COMMENTS,
    TON_ASSET_ID,
    EvaaSupplyBlock,
    EvaaWithdrawBlock,
    build_evaa_supply_core,
    load_user_header,
)
from indexer.events.blocks.messages.evaa import (
    EvaaLiquidateFail,
    EvaaLiquidateMaster,
    EvaaLiquidateSatisfied,
    EvaaLiquidateSuccess,
    EvaaLiquidateSuccessReport,
    EvaaLiquidateUnsatisfied,
    EvaaLiquidateUser,
    EvaaSupplyFail,
    EvaaSupplyMaster,
    EvaaSupplySuccess,
    EvaaSupplyUser,
    EvaaWithdrawCollateralized,
    EvaaWithdrawFail,
    EvaaWithdrawFailExcess,
    EvaaWithdrawMaster,
    EvaaWithdrawNoFundsExcess,
    EvaaWithdrawSuccess,
    EvaaWithdrawUser,
)
from indexer.events.blocks.utils import AccountId
from indexer.events.mch.registry import Registries

# The four "fail on user contract" error opcodes have no per-opcode message
# classes; the literals are pinned against the host list they came from.
_FAIL_EXCESS_OPCODES = {
    "EvaaWithdrawLockedExcess":            0x21e6,
    "EvaaWithdrawNotCollateralizedExcess": 0x21e7,
    "EvaaWithdrawMissingPricesExcess":     0x21e8,
    "EvaaWithdrawExecutionCrashed":        0x21ec,
}
assert sorted(_FAIL_EXCESS_OPCODES.values()) == sorted(EvaaWithdrawFailExcess.opcodes)

OPCODES = {
    "EvaaSupplySuccess":          EvaaSupplySuccess.opcode,
    "EvaaSupplyFail":             EvaaSupplyFail.opcode,
    "EvaaWithdrawMaster":         EvaaWithdrawMaster.opcode,
    "EvaaWithdrawCollateralized": EvaaWithdrawCollateralized.opcode,
    "EvaaWithdrawSuccess":        EvaaWithdrawSuccess.opcode,
    "EvaaWithdrawNoFundsExcess":  EvaaWithdrawNoFundsExcess.opcode,
    "EvaaLiquidateMaster":        EvaaLiquidateMaster.opcode,
    "EvaaLiquidateSatisfied":     EvaaLiquidateSatisfied.opcode,
    "EvaaLiquidateUnsatisfied":   EvaaLiquidateUnsatisfied.opcode,
    "EvaaLiquidateSuccessReport": EvaaLiquidateSuccessReport.opcode,
    "EvaaLiquidateFail":          EvaaLiquidateFail.opcode,
    **_FAIL_EXCESS_OPCODES,
}

BLOCK_TYPES = {
    "EvaaSupplyBlock":    EvaaSupplyBlock,
    "EvaaWithdrawBlock":  EvaaWithdrawBlock,
    "evaa_liquidate":     Block,
}

MESSAGE_TYPES = {
    "EvaaWithdrawMaster":         EvaaWithdrawMaster,
    "EvaaWithdrawCollateralized": EvaaWithdrawCollateralized,
    "EvaaWithdrawFailExcess":     EvaaWithdrawFailExcess,
}


def _opcode_after_user_header(block: Block, opcode: int) -> bool:
    """EvaaContractWithHeaderMatcher.test_self equivalent: skip the user
    header (version coins + maybe upgrade-info ref + 2 exec bits), then
    compare the opcode. Malformed bodies never match."""
    if not isinstance(block, CallContractBlock):
        return False
    try:
        body = block.get_body()
        load_user_header(body)
        return body.load_uint(32) == opcode
    except Exception:
        return False


def _is_supply_anchor(block: Block) -> bool:
    """EvaaSupplyBlockMatcher.test_self equivalent: the 0x1 supply_master call
    itself (TON side), or a jetton transfer whose FORWARD PAYLOAD starts with the
    supply_master opcode (jetton side). btype, not isinstance — the legacy
    JettonTransferBlock check is dead under the IR engine."""
    if isinstance(block, CallContractBlock) and block.opcode == EvaaSupplyMaster.opcode:
        return True
    if block.btype != "jetton_transfer":
        return False
    try:
        payload = block.data.get("forward_payload")
        if not payload:
            return False
        payload_slice = Cell.from_boc(payload)[0].begin_parse()
        return payload_slice.load_uint(32) == EvaaSupplyMaster.opcode
    except Exception:
        return False


def _is_liquidate_anchor(block: Block) -> bool:
    if isinstance(block, CallContractBlock) and block.opcode == EvaaLiquidateMaster.opcode:
        return True
    if block.btype != "jetton_transfer":
        return False
    try:
        payload = block.data.get("forward_payload")
        if not payload:
            return False
        return (Cell.from_boc(payload)[0].begin_parse().load_uint(32)
                == EvaaLiquidateMaster.opcode)
    except Exception:
        return False


def _is_bounced_call(block: Block) -> bool:
    return isinstance(block, CallContractBlock) and block.get_message().bounced


def _liquidate_result(liquidator, borrower, borrower_contract, collateral_asset_id,
                      collateral_amount, is_success, fail_reason, debt_amount,
                      debt_asset_id):
    return dict(zip(
        ("liquidator", "borrower", "borrower_contract", "collateral_asset_id",
         "collateral_amount", "is_success", "fail_reason", "debt_amount",
         "debt_asset_id"),
        (liquidator, borrower, borrower_contract, collateral_asset_id,
         collateral_amount, is_success, fail_reason, debt_amount, debt_asset_id),
    ))


async def _evaa_liquidate_data(consumed: list[Block]):
    """Build the four legacy liquidation outcomes from the consumed set."""
    if not consumed:
        return None
    block, rest = consumed[0], consumed[1:]
    try:
        is_ton = isinstance(block, CallContractBlock)
        if is_ton:
            liquidator = AccountId(block.get_message().source)
            master = EvaaLiquidateMaster(block.get_body())
            transferred_asset_id = TON_ASSET_ID
        else:
            liquidator = AccountId(block.data["sender"])
            payload = block.data.get("forward_payload")
            if not payload:
                return None
            master = EvaaLiquidateMaster(Cell.from_boc(payload)[0].begin_parse())
            transferred_asset_id = block.data["receiver_wallet"].address
        borrower = AccountId(master.borrower_address)

        user = next((b for b in rest
                     if _opcode_after_user_header(b, EvaaLiquidateUser.opcode)), None)
        if user is None:
            immediate = next((b for b in rest
                              if _is_bounced_call(b) or b.btype == "jetton_transfer"), None)
            if immediate is None:
                return None
            return _liquidate_result(
                liquidator, borrower, None, master.collateral_asset_id, 0, False,
                "immediate_rejection", master.liquidate_incoming_amount,
                transferred_asset_id)

        borrower_contract = AccountId(user.get_message().destination)
        satisfied = next((b for b in rest
                          if isinstance(b, CallContractBlock)
                          and b.opcode == EvaaLiquidateSatisfied.opcode), None)
        unsatisfied = next((b for b in rest
                            if isinstance(b, CallContractBlock)
                            and b.opcode == EvaaLiquidateUnsatisfied.opcode), None)
        satisfied_success = satisfied is not None and any(
            _opcode_after_user_header(c, EvaaLiquidateSuccess.opcode)
            for c in satisfied.next_blocks)
        satisfied_fail = satisfied is not None and any(
            isinstance(c, CallContractBlock) and c.opcode == EvaaLiquidateFail.opcode
            for c in satisfied.next_blocks)

        if satisfied_success:
            parsed = EvaaLiquidateSatisfied(satisfied.get_body())
            return _liquidate_result(
                liquidator, AccountId(parsed.owner_address), borrower_contract,
                parsed.collateral_asset_id, parsed.collateral_reward, True, None,
                parsed.liquidatable_amount, parsed.transferred_asset_id)
        if unsatisfied is not None:
            try:
                reason = EvaaLiquidateUnsatisfied(unsatisfied.get_body()).get_error().reason
            except Exception:
                reason = "liquidation_error"
            return _liquidate_result(
                liquidator, borrower, borrower_contract, master.collateral_asset_id,
                0, False, reason, master.liquidate_incoming_amount,
                transferred_asset_id)
        if satisfied_fail:
            return _liquidate_result(
                liquidator, borrower, borrower_contract, master.collateral_asset_id,
                0, False, "master_not_enough_liquidity",
                master.liquidate_incoming_amount, transferred_asset_id)
        return None
    except Exception:
        return None


async def _evaa_supply_data(consumed: list[Block]):
    """Host residue of the evaa supply build (hybrid declarative spec). Derives
    the legacy label roles structurally from `Match._mch_consumed` and delegates
    to build_evaa_supply_core, which the legacy matcher also calls. Returns the
    EvaaSupplyData record the `out` block flattens, or None to reject (§10.7)."""
    block, rest = consumed[0], consumed[1:]
    try:
        user = next((b for b in rest
                     if _opcode_after_user_header(b, EvaaSupplyUser.opcode)), None)
        success = next((b for b in rest
                        if isinstance(b, CallContractBlock)
                        and b.opcode == EvaaSupplySuccess.opcode), None)
        fail = next((b for b in rest
                     if isinstance(b, CallContractBlock)
                     and b.opcode == EvaaSupplyFail.opcode), None)
        refund = next((b for b in rest if b.btype == "jetton_transfer"), None)
        return await build_evaa_supply_core(block, user, success, fail, refund, rest)
    except Exception:
        return None


def register_evaa(reg: Registries) -> Registries:
    """Register everything specs/evaa.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)
    reg.message_types.update(MESSAGE_TYPES)

    @reg.register_predicate("evaa_user_withdraw_user")
    def evaa_user_withdraw_user(block: Block) -> bool:
        return _opcode_after_user_header(block, EvaaWithdrawUser.opcode)

    @reg.register_predicate("evaa_user_withdraw_success")
    def evaa_user_withdraw_success(block: Block) -> bool:
        return _opcode_after_user_header(block, EvaaWithdrawSuccess.opcode)

    @reg.register_predicate("evaa_user_withdraw_fail")
    def evaa_user_withdraw_fail(block: Block) -> bool:
        return _opcode_after_user_header(block, EvaaWithdrawFail.opcode)

    @reg.register_predicate("evaa_supply_anchor")
    def evaa_supply_anchor(block: Block) -> bool:
        return _is_supply_anchor(block)

    @reg.register_predicate("evaa_user_supply")
    def evaa_user_supply(block: Block) -> bool:
        return _opcode_after_user_header(block, EvaaSupplyUser.opcode)

    @reg.register_predicate("evaa_service_comment")
    def evaa_service_comment(block: Block) -> bool:
        return (isinstance(block, TonTransferBlock)
                and block.comment in EVAA_ACTION_COMMENTS)

    @reg.register_predicate("evaa_liquidate_anchor")
    def evaa_liquidate_anchor(block: Block) -> bool:
        return _is_liquidate_anchor(block)

    @reg.register_predicate("evaa_user_liquidate")
    def evaa_user_liquidate(block: Block) -> bool:
        return _opcode_after_user_header(block, EvaaLiquidateUser.opcode)

    @reg.register_predicate("evaa_liquidate_success_header")
    def evaa_liquidate_success_header(block: Block) -> bool:
        return _opcode_after_user_header(block, EvaaLiquidateSuccess.opcode)

    @reg.register_predicate("evaa_bounced_call")
    def evaa_bounced_call(block: Block) -> bool:
        return _is_bounced_call(block)

    reg.register_fn("evaa_supply_data")(_evaa_supply_data)
    reg.register_fn("evaa_liquidate_data")(_evaa_liquidate_data)
    return reg


def evaa_registries() -> Registries:
    """Fresh Registries ready to compile specs/evaa.mch."""
    return register_evaa(Registries())
