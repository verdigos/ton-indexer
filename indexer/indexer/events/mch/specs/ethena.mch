# Ethena deposit matcher — ported from blocks/ethena_staking.py
# EthenaDepositBlockMatcher. Anchors on an already-built `jetton_transfer`
# block (first btype-anchored matcher in the spec set — the engine anchor test
# is btype-set membership) whose child is the pool's `jetton_mint`. The
# hardcoded USDe/tsUSDe asset gate lives in the host build core (returns None to
# reject). EthenaWithdrawalRequestBlockMatcher stays hand-written (out of scope).

matcher ethena_deposit {
    produces EthenaDepositBlock
    build    build_ethena_deposit
    entry    @transfer

    btype jetton_transfer @transfer -> btype jetton_mint @mint
}
