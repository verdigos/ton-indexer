# JVault family — R02 DECLARATIVE rewrite (M4 build language).
#
# Supersedes the consumed-set-driven builder tier (builders/jvault.py used to
# host four `build_jvault_*` adapters over the cores in blocks/jvault.py). The
# structural patterns are UNCHANGED from the R02 remake — same anchors, same
# `$jvault_referrals` single-level recursion, same consumed sets — so block
# topology and value_flow follow automatically (every build path still ends in
# merge_blocks(consumed)). Only the build bodies moved into the language:
#
# - extract_jvault_assets (two chained interface-repo reads + a data-cell parse)
#   is now the record-returning lookup `jvault_assets` (spec §15b G10: one
#   lookup, one record, read its fields). First real use of the sanctioned
#   record-lookup pattern — see builders/jvault.py.
# - unstake / unstake_request success/rejection gate on the lookup record and
#   on `msg.exit_code` (the defined envelope field) — no host build fn.
# - claim's claimed_jettons/claimed_amounts are a host list-transform fn + a
#   one-element list literal (the constructor-map gap is deferred G-LC, §16.1).
# - stake's forward_payload sub-cell parse stays a host fn (jvault_stake_period)
#   — the sanctioned irreducible escape (§15b: protocol-specific cell parsing).
#
# Produced btype is the bare string (jvault_stake, …) rather than the class
# name: the class→btype normalization map (mch/btypes.py) does not carry the
# JVault classes, and the serializer + legacy blocks key on the bare btype.

opcode JVaultReceiveJettons        = 0xD68A4AC1
opcode JVaultRequestUpdateRewards  = 0xF5C5BAA3
opcode JVaultUpdateRewards         = 0xAE9307CE
opcode JVaultRequestUpdateReferrer = 0x55C35B40
opcode JVaultUpdateReferrer        = 0x076EE4E0
opcode JVaultSetData               = 0x383411EA
opcode JVaultStakeCancel           = 0x9EADA1D9
opcode JVaultUnstakeJettons        = 0x499A9262
opcode JVaultUnstakeRequest        = 0x0168D4B7
opcode JVaultClaim                 = 0x78D9F109
opcode JVaultSendClaimedRewards    = 0x44BC1FE3

# Pool referrer-update fan-out: a linear chain of request/update referrer
# messages (each auto-consuming its excess/bounce), terminated by exhaustion.
rule jvault_referrals = (
    op JVaultRequestUpdateReferrer @ref -> maybe $jvault_referrals
  ^ op JVaultUpdateReferrer @refupd -> maybe $jvault_referrals
)

matcher jvault_stake {
    produces jvault_stake
    entry    @transfer

    btype jetton_transfer @transfer
      -> { op JVaultReceiveJettons @receive
             -> ( op JVaultRequestUpdateRewards @request
                    -> { maybe $jvault_referrals ; op JVaultUpdateRewards @update }
                | op JVaultStakeCancel @cancel
                    -> maybe btype jetton_transfer @cancel_transfer )
         ; maybe op JVaultSetData @setdata }

    # Legacy build_jvault_stake_core, field-for-field. The staker + asset come
    # off the already-built incoming jetton_transfer's `data`; the stake wallet
    # and pool are the receive call's envelope. The staked amount and stake
    # period both live on the transfer's parsed message (amount is a scalar; the
    # period is a forward_payload sub-cell) — both reached via host fn, since the
    # composed block's `.jetton_transfer_message` is invisible to expressions and
    # re-`parse`ing its get_body does not round-trip.
    reject when cancel == null and request == null
    failed when receive.failed
    failed when cancel != null
    failed when (request != null) and request.failed
    out {
        sender:        account(transfer.data.sender)
        sender_wallet: account(transfer.data.sender_wallet)
        asset:         transfer.data.asset
        stake_wallet:  account(receive.msg.destination)
        staking_pool:  account(receive.msg.source)
        staked_amount: jvault_transfer_amount(transfer)
        period:        jvault_stake_period(transfer)
    }
}

matcher jvault_unstake {
    produces jvault_unstake
    entry    @unstake

    op JVaultUnstakeJettons @unstake
      -> maybe ( op JVaultRequestUpdateRewards @request
                   -> { maybe $jvault_referrals
                      ; maybe btype ton_transfer @fee
                      ; btype jetton_transfer @withdraw
                      ; op JVaultUpdateRewards @update } )

    # Legacy build_jvault_unstake_core: assets resolved from the stake wallet
    # (record lookup); reject if the pool/asset can't be resolved. Two output
    # shapes — early (no pool round-trip: pool+asset from the lookup, exit code
    # from the anchor) vs full (pool from the request, asset from the withdraw
    # leg, fee from the optional ton leg).
    parse unstake as JVaultUnstakeJettons
    let assets = lookup jvault_assets(unstake.msg.destination)
    let early  = request == null or withdraw == null
    reject when assets == null
    reject when assets.asset == null
    reject when (not early) and (assets.asset != withdraw.data.asset)
    out {
        sender:            account(unstake.msg.source)
        stake_wallet:      account(unstake.msg.destination)
        staking_pool:      account(request.msg.destination) if not early else assets.staking_pool
        unstaked_amount:   unstake.body.jettons_to_unstake
        unstake_fee_taken: (fee.msg.value ?? 0) if not early else null
        asset:             withdraw.data.asset if not early else assets.asset
        jvault_asset:      assets.jvault_asset
        exit_code:         null if not early else unstake.msg.exit_code
    }
}

matcher jvault_claim {
    produces jvault_claim
    entry    @claim

    op JVaultClaim @claim
      -> op JVaultSendClaimedRewards @send
           -> { btype jetton_transfer @withdraw ; op JVaultUpdateRewards @update }

    # Legacy build_jvault_claim_core. claimed_jettons is the claim body's
    # jetton-wallet dict mapped through the AccountId constructor (host fn —
    # constructor-map is deferred G-LC); claimed_amounts is the single withdraw
    # leg's raw transfer amount as a one-element list literal (the amount read is
    # a host fn — a composed jetton_transfer's message can't be re-`parse`d).
    parse claim as JVaultClaim
    out {
        sender:          account(claim.msg.source)
        stake_wallet:    account(claim.msg.destination)
        staking_pool:    account(send.msg.destination)
        claimed_jettons: jvault_account_list(claim.body.jettons_to_claim)
        claimed_amounts: [ jvault_transfer_amount(withdraw) ]
    }
}

matcher jvault_unstake_request {
    produces jvault_unstake_request
    entry    @request_anchor

    op JVaultUnstakeRequest @request_anchor
      -> maybe ( op JVaultRequestUpdateRewards @request
                   -> { maybe $jvault_referrals ; op JVaultUpdateRewards @update } )

    # Legacy build_jvault_unstake_request_core. Two shapes: early (request never
    # reached the pool → failed, exit code from the anchor, pool+assets from the
    # lookup, rejected if unresolvable) vs full (pool from the request, assets
    # still from the lookup but NOT reject-gated, failed OR'd from both calls).
    parse request_anchor as JVaultUnstakeRequest
    let assets = lookup jvault_assets(request_anchor.msg.destination)
    let early  = request == null
    reject when early and assets == null
    reject when early and assets.asset == null
    failed when early
    failed when (not early) and (request_anchor.failed or request.failed)
    out {
        sender:           account(request_anchor.msg.source)
        stake_wallet:     account(request_anchor.msg.destination)
        staking_pool:     assets.staking_pool if early else account(request.msg.destination)
        requested_amount: request_anchor.body.jettons_to_unstake
        asset:            assets.asset
        jvault_asset:     assets.jvault_asset
        exit_code:        request_anchor.msg.exit_code if early else null
    }
}
