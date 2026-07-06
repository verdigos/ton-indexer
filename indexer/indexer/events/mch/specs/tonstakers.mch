# Tonstakers withdraw matcher — ported from blocks/staking.py
# TONStakersWithdrawMatcher. HYBRID declarative build. Anchors on the raw
# JettonBurn call (runs before the jetton matchers, so the burn is still a plain
# call_contract), then a fixed child chain to the pool's withdraw request, which
# forks into an immediate pool withdrawal or a delayed withdrawal that mints a
# request NFT.
#
# produces switch (T11/M4, spec §9.5): the fork is an XOR of two distinct
# concrete heads; the switch dispatches the produced btype on which arm bound —
# tonstakers_withdraw (immediate) vs tonstakers_withdraw_request (delayed). Both
# arms map their fields declaratively; two host fns cover the genuinely
# inexpressible residue (each a named language gap):
#
#   tonstakers_minted_nft(delayed) — GAP: reading the minted request NFT's
#     address off delayed.next_blocks is block-topology navigation, deliberately
#     excluded from the language (§15b). The NFT is a NON-consumed neighbor
#     (NftMintBlockMatcher produced it as its own sibling nft_mint action);
#     capturing it as a pattern node would consume it and diverge the consumed
#     set, so it must be read by a host fn, exactly as the legacy core does.

opcode JettonBurn                = 0x595f07bc
opcode JettonBurnNotification    = 0x7bdd97de
opcode TONStakersWithdrawRequest = 0x319b0cdc
opcode TONStakersPoolWithdrawal  = 0x0a77535c
opcode TONStakersMintNFT         = 0x1674b0a0

matcher tonstakers_withdraw {
    produces tonstakers_withdraw | tonstakers_withdraw_request
    entry    @burn

    op JettonBurn @burn
      -> op JettonBurnNotification
        -> op TONStakersWithdrawRequest @request
          -> ( op TONStakersPoolWithdrawal @immediate_withdrawal
             ^ op TONStakersMintNFT @delayed_withdrawal )

    parse burn as JettonBurn
    let minted = tonstakers_minted_nft(delayed_withdrawal)
    failed when burn.failed
    failed when immediate_withdrawal == null and minted == null
    produces switch {
        when immediate_withdrawal != null => tonstakers_withdraw out {
            stake_holder: account(burn.msg.source)
            burnt_nft:    null
            pool:         account(request.msg.destination)
            tokens_burnt: amount(burn.body.amount)
            amount:       amount(immediate_withdrawal.msg.value - request.msg.value)
            asset:        asset(request.msg.source)
        }
        else => tonstakers_withdraw_request out {
            source:       account(burn.msg.source)
            tsTON_wallet: account(burn.msg.destination)
            pool:         account(request.msg.destination)
            tokens_burnt: amount(burn.body.amount)
            minted_nft:   minted
            asset:        asset(request.msg.source)
        }
    }
}
