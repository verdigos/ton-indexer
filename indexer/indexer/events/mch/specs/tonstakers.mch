# Tonstakers withdraw matcher — ported from blocks/staking.py
# TONStakersWithdrawMatcher. Anchors on the raw JettonBurn call (runs before the
# jetton matchers, so the burn is still a plain call_contract), then a fixed
# child chain to the pool's withdraw request, which forks into an immediate
# pool withdrawal or a delayed withdrawal that mints a request NFT.
#
# produces-union (T11, spec §8.1): the build returns TONStakersWithdrawBlock for
# the immediate arm and TONStakersWithdrawRequestBlock for the delayed arm. The
# fork is an XOR of two distinct concrete heads — behaviorally an ordered
# OrMatcher (legacy), inert exclusivity as an edge target (spec §6.6).
#
# The minted NFT (delayed arm) is NOT a pattern node: the legacy matcher reads it
# off delayed_withdrawal.next_blocks and leaves it as a separate nft_mint action.
# Capturing it would consume it and diverge from the legacy consumed set, so the
# build core keeps the next_blocks read (NftMintBlockMatcher runs first).

opcode JettonBurn                = 0x595f07bc
opcode JettonBurnNotification    = 0x7bdd97de
opcode TONStakersWithdrawRequest = 0x319b0cdc
opcode TONStakersPoolWithdrawal  = 0x0a77535c
opcode TONStakersMintNFT         = 0x1674b0a0

matcher tonstakers_withdraw {
    produces TONStakersWithdrawBlock | TONStakersWithdrawRequestBlock
    build    build_tonstakers_withdraw
    entry    @burn

    op JettonBurn @burn
      -> op JettonBurnNotification
        -> op TONStakersWithdrawRequest @request
          -> ( op TONStakersPoolWithdrawal @immediate_withdrawal
             ^ op TONStakersMintNFT @delayed_withdrawal )
}
