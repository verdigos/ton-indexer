# Dedust withdraw liquidity (T17) — ported from blocks/liquidity.py
# DedustWithdrawBlockMatcher.
#
# Shape of interest: a children set with two IDENTICAL-head items (both
# `op DedustPayoutFromPool`). The engine's children-set algorithm (spec §6.5)
# is first-match-wins over candidates in canonical order, so item 1 consumes
# the first matching payout chain and item 2 the next — deterministic, and it
# matches the legacy labeled('payout_1')/labeled('payout_2') assignment
# exactly. The @payout_1/@payout_2 captures bind positionally the same way
# (no recursion anywhere in this pattern), so the builder is fully
# capture-driven — no `_mch_consumed` needed.
#
# Each payout terminal is either a TON payout (`op DedustPayout`) or a
# jetton payout (`btype jetton_transfer`, produced by the prerequisite
# JettonTransferBlockMatcher/PTonTransferMatcher — same registration-order
# dependency as legacy). The duplicated capture name across the two branches
# is legal: bare concrete nodes with distinct heads (heuristic-exclusive OR,
# spec §6.2/§6.6).

opcode JettonBurn             = 0x595f07bc
opcode JettonBurnNotification = 0x7bdd97de
opcode DedustPayoutFromPool   = 0xad4eb6f5
opcode DedustPayout           = 0x474f86cf

matcher dedust_withdraw {
    produces dex_withdraw_liquidity
    build    build_dedust_withdraw
    entry    @burn

    op JettonBurn @burn
      -> op JettonBurnNotification @notification
      -> { op DedustPayoutFromPool -> ( op DedustPayout @payout_1 | btype jetton_transfer @payout_1 )
         ; op DedustPayoutFromPool -> ( op DedustPayout @payout_2 | btype jetton_transfer @payout_2 ) }
}
