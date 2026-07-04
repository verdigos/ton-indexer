"""mch builder for the evaa_withdraw matcher (specs/evaa.mch).

Thin adapter over the shared core `build_evaa_withdraw_core` in
blocks/evaa.py — the same function the legacy EvaaWithdrawBlockMatcher calls.

The builder is consumed-set-driven (`Match._mch_consumed`), NOT capture-driven
— same situation as stonfi_v2/dedust_swap: the compiled path's positional
capture walker resolves an alternative by the first branch whose HEAD test
passes, and the two collateralized branches share the head `op
EvaaWithdrawCollateralized` — on a fail trace it binds @coll_ok instead of
@coll_fail (the v2 IR engine binds during the walk and is not affected).
`_derive_parts` reconstructs the legacy label roles structurally; every role
block carries a unique opcode signature, so the derivation is unambiguous
(see the function's docstring).

Host predicates (each one a dual-implementation parity liability, spec §5.2):

- evaa_user_withdraw_user / evaa_user_withdraw_success /
  evaa_user_withdraw_fail — EVAA user-contract messages carry a
  variable-length header before the 32-bit opcode, so a plain `op` test cannot
  identify them. `_opcode_after_user_header` replicates
  EvaaContractWithHeaderMatcher.test_self (blocks/evaa.py) byte for byte.
- evaa_service_comment — legacy evaa_action_comment_matcher: a TonTransferBlock
  whose comment is one of the EVAA service comments (set membership over a
  block attribute; not expressible as an M4 inline field test).
"""
from __future__ import annotations

from indexer.events.blocks.basic_blocks import CallContractBlock, TonTransferBlock
from indexer.events.blocks.core import Block
from indexer.events.blocks.evaa import (
    EVAA_ACTION_COMMENTS,
    EvaaWithdrawBlock,
    build_evaa_withdraw_core,
    load_user_header,
)
from indexer.events.blocks.jettons import JettonTransferBlock
from indexer.events.blocks.messages.evaa import (
    EvaaWithdrawCollateralized,
    EvaaWithdrawFail,
    EvaaWithdrawFailExcess,
    EvaaWithdrawMaster,
    EvaaWithdrawNoFundsExcess,
    EvaaWithdrawSuccess,
    EvaaWithdrawUser,
)
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
    "EvaaWithdrawMaster":         EvaaWithdrawMaster.opcode,
    "EvaaWithdrawCollateralized": EvaaWithdrawCollateralized.opcode,
    "EvaaWithdrawSuccess":        EvaaWithdrawSuccess.opcode,
    "EvaaWithdrawNoFundsExcess":  EvaaWithdrawNoFundsExcess.opcode,
    **_FAIL_EXCESS_OPCODES,
}

BLOCK_TYPES = {
    "EvaaWithdrawBlock": EvaaWithdrawBlock,
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


def _derive_parts(consumed: list[Block]):
    """Reconstruct the legacy label roles from the consumed set.

    Every role has a unique signature within one match's consumed set:
    withdraw_on_user / the user-side success update / the user-side fail
    revert carry header-prefixed opcodes (0x21 / 0x211a / 0x211f); the TON
    payout is the only PLAIN-opcode 0x211a call (the header-prefixed update's
    raw first-32-bits differ); the jetton payout is the only
    JettonTransferBlock; the fail-excess block is the only member of the
    4-opcode error set. Legacy's success-vs-fail label split maps to which
    user-side follow-up was consumed: the collateralized block is
    `withdraw_collateralized_success` iff the success update is present,
    `withdraw_collateralized_fail` iff the fail revert is (the two branches
    are mutually exclusive subtrees).
    """
    def first_call(pred):
        return next((b for b in consumed
                     if isinstance(b, CallContractBlock) and pred(b)), None)

    anchor = consumed[0]
    user = next((b for b in consumed
                 if _opcode_after_user_header(b, EvaaWithdrawUser.opcode)), None)
    coll = first_call(lambda b: b.opcode == EvaaWithdrawCollateralized.opcode)
    user_success = next((b for b in consumed
                         if _opcode_after_user_header(b, EvaaWithdrawSuccess.opcode)), None)
    fail_no_funds = next((b for b in consumed
                          if _opcode_after_user_header(b, EvaaWithdrawFail.opcode)), None)
    ton_payout = first_call(lambda b: b.opcode == EvaaWithdrawSuccess.opcode)
    jetton_payout = next((b for b in consumed
                          if isinstance(b, JettonTransferBlock)), None)
    fail_excess = first_call(lambda b: b.opcode in EvaaWithdrawFailExcess.opcodes)

    coll_ok = coll if user_success is not None else None
    coll_fail = coll if (user_success is None and fail_no_funds is not None) else None
    return anchor, user, coll_ok, ton_payout, jetton_payout, coll_fail, fail_excess


def register_evaa(reg: Registries) -> Registries:
    """Register everything specs/evaa.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_predicate("evaa_user_withdraw_user")
    def evaa_user_withdraw_user(block: Block) -> bool:
        return _opcode_after_user_header(block, EvaaWithdrawUser.opcode)

    @reg.register_predicate("evaa_user_withdraw_success")
    def evaa_user_withdraw_success(block: Block) -> bool:
        return _opcode_after_user_header(block, EvaaWithdrawSuccess.opcode)

    @reg.register_predicate("evaa_user_withdraw_fail")
    def evaa_user_withdraw_fail(block: Block) -> bool:
        return _opcode_after_user_header(block, EvaaWithdrawFail.opcode)

    @reg.register_predicate("evaa_service_comment")
    def evaa_service_comment(block: Block) -> bool:
        return (isinstance(block, TonTransferBlock)
                and block.comment in EVAA_ACTION_COMMENTS)

    @reg.register_builder("build_evaa_withdraw")
    async def build_evaa_withdraw(m):
        (anchor, user, coll_ok, ton_payout, jetton_payout,
         coll_fail, fail_excess) = _derive_parts(m._mch_consumed)
        return await build_evaa_withdraw_core(
            anchor, user, coll_ok, ton_payout, jetton_payout,
            coll_fail, fail_excess,
        )

    return reg


def evaa_registries() -> Registries:
    """Fresh Registries ready to compile specs/evaa.mch."""
    return register_evaa(Registries())
