# Stonfi v2 withdraw liquidity (R04 remake) — remade from blocks/liquidity.py
# StonfiV2WithdrawLiquidityMatcher.
#
# The legacy matcher anchors on ONE pool pay-to (op 0x657b54f5) and then does an
# imperative sibling scan (block.previous_block.next_blocks) to pick up the
# second pay-to — a workaround because a single op appears twice under the
# router. The remake drops the scan: the two pay-tos are a children set under
# their common parent, bound positionally (@payout_1/@payout_2), exactly like
# dedust_withdraw (T17). first-match-wins over candidates in canonical order
# reproduces the legacy [anchor]+siblings order.
#
# Legacy's OrMatcher parent (normal burn vs refund) splits into two matchers —
# no shared structural head exists (normal is fronted by a JettonBurn, refund by
# op 0x132b9a2c) and multi-kind anchors (R09) are not available. The anchors are
# mutually exclusive, so on any one trace at most one fires, and each produces
# the same single dex_withdraw_liquidity block the legacy matcher does.
#
# Normal: burn-anchored (btype jetton_burn, the JettonBurnBlockMatcher-produced
# block; stonfi_v2 runs after it in event_processing) -> op 0x297437cf router ->
# {two pay-tos}. Same shape family as dedust_withdraw.
# Refund: anchored on op 0x0f98e2b8, its parent (op 0x132b9a2c) carried on a
# `<-` edge (wrapped-children-set CB exemption, §6.5 / tonco precedent) so the
# core can read the sender off refund.previous_block, and the parent is consumed.
#
# Each pay-to has a jetton_transfer child; a TON payout carries a PTonTransfer
# grandchild under that transfer (`maybe op PTonTransfer`) — legacy reads the
# ton_amount off it and marks the leg is_ton. The captures bind positionally
# (no recursion), so the builder is capture-driven, delegating to the shared
# build_stonfi_v2_withdraw_core (same fn the legacy matcher now calls).

opcode StonfiV2PayTo                 = 0x657b54f5
opcode StonfiV2WithdrawFromPool      = 0x297437cf
opcode StonfiV2WithdrawRefund        = 0x0f98e2b8
opcode StonfiV2WithdrawRefundParent  = 0x132b9a2c
opcode PTonTransfer                  = 0x01f3835d

matcher stonfi_v2_withdraw {
    produces dex_withdraw_liquidity
    build    build_stonfi_v2_withdraw
    entry    @burn

    btype jetton_burn @burn
      -> op StonfiV2WithdrawFromPool @router
           -> { op StonfiV2PayTo @payout_1 -> btype jetton_transfer @jt_1 -> maybe op PTonTransfer @pton_1
              ; op StonfiV2PayTo @payout_2 -> btype jetton_transfer @jt_2 -> maybe op PTonTransfer @pton_2 }
}

matcher stonfi_v2_withdraw_refund {
    produces dex_withdraw_liquidity
    build    build_stonfi_v2_withdraw_refund
    entry    @refund

    op StonfiV2WithdrawRefund @refund
      -> { op StonfiV2PayTo @payout_1 -> btype jetton_transfer @jt_1 -> maybe op PTonTransfer @pton_1
         ; op StonfiV2PayTo @payout_2 -> btype jetton_transfer @jt_2 -> maybe op PTonTransfer @pton_2 }
      <- op StonfiV2WithdrawRefundParent @refund_parent
}
