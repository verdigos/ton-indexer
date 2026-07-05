# Ethena deposit matcher — ported from blocks/ethena_staking.py
# EthenaDepositBlockMatcher. Anchors on an already-built `jetton_transfer`
# block (first btype-anchored matcher in the spec set — the engine anchor test
# is btype-set membership) whose child is the pool's `jetton_mint`.
# EthenaWithdrawalRequestBlockMatcher stays hand-written (out of scope).
#
# Fully declarative build (M4 §9): the hardcoded USDe/tsUSDe asset gate is two
# `reject when` clauses (Asset compares case-insensitively against the master
# address literal — a TON/absent asset raises, i.e. rejects, exactly like the
# legacy None guards). Every field is read off the composed jetton_transfer /
# jetton_mint blocks' produced `data` (§9.6 `.data` accessor).

matcher ethena_deposit {
    produces EthenaDepositBlock
    entry    @transfer

    btype jetton_transfer @transfer -> btype jetton_mint @mint

    reject when transfer.data.asset != "0:086FA2A675F74347B08DD4606A549B8FDB98829CB282BC1949D3B12FBAED9DCC"
    reject when mint.data.asset     != "0:D0E545323C7ACB7102653C073377F7E3C67F122EB94D430A250739F109D4A57D"
    out {
        source:             account(transfer.data.sender)
        user_jetton_wallet: account(transfer.data.sender_wallet)
        pool:               account(transfer.data.receiver)
        value:              amount(transfer.data.amount)
        tokens_minted:      amount(mint.data.amount)
        asset:              mint.data.asset
        source_asset:       transfer.data.asset
    }
}
