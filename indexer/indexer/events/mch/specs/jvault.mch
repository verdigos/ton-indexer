# JVault family (R02 remake) — remade from blocks/jvault.py:
#   JVaultStakeBlockMatcher / JVaultUnstakeBlockMatcher /
#   JVaultClaimBlockMatcher / JVaultUnstakeRequestBlockMatcher.
#
# Remake gate (docs/mch-roadmap.md M2R): the internal pattern is free, OUTPUT is
# identical — produced btype, `data`, failed/broken, value_flow, consumed
# event-node set, block topology. Topology + value_flow follow automatically from
# the consumed set because every build core ends in `merge_blocks(consumed)`, so
# the only thing the pattern must reproduce is WHICH blocks are consumed.
#
# Legacy shape vs remake:
# - Legacy models the pool's referrer-update fan-out as a NESTED RecursiveMatcher
#   (referral_chain = repeating(referral_subchain); referral_subchain =
#   repeating(RequestUpdateReferrer) exit(UpdateReferrer)). Per fixture the real
#   flow is a single linear chain `RequestUpdateReferrer* UpdateReferrer?` hanging
#   off the pool's request_update_rewards, so it flattens to ONE recursion level
#   ($jvault_referrals below) — the port-wave finding for R02.
# - Every `op` node auto-includes excess (0xd53276db) and bounces (0xffffffff),
#   same as the legacy ContractMatcher(include_excess=True) defaults, so the
#   trailing excess/bounce of each referral / update message is consumed.
#
# Builders are consumed-set-driven (Match._mch_consumed), same convention as
# specs/evaa.mch: each role block carries a unique opcode/btype within one
# match's consumed set, so builders/jvault.py reconstructs the legacy participant
# roles unambiguously and delegates to the shared build_*_core in blocks/jvault.py
# (the same cores the thin legacy matchers now call).

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
    produces JVaultStakeBlock
    build    build_jvault_stake
    entry    @transfer

    btype jetton_transfer @transfer
      -> { op JVaultReceiveJettons @receive
             -> ( op JVaultRequestUpdateRewards @request
                    -> { maybe $jvault_referrals ; op JVaultUpdateRewards @update }
                | op JVaultStakeCancel @cancel
                    -> maybe btype jetton_transfer @cancel_transfer )
         ; maybe op JVaultSetData @setdata }
}

matcher jvault_unstake {
    produces JVaultUnstakeBlock
    build    build_jvault_unstake
    entry    @unstake

    op JVaultUnstakeJettons @unstake
      -> maybe ( op JVaultRequestUpdateRewards @request
                   -> { maybe $jvault_referrals
                      ; maybe btype ton_transfer @fee
                      ; btype jetton_transfer @withdraw
                      ; op JVaultUpdateRewards @update } )
}

matcher jvault_claim {
    produces JVaultClaimBlock
    build    build_jvault_claim
    entry    @claim

    op JVaultClaim @claim
      -> op JVaultSendClaimedRewards @send
           -> { btype jetton_transfer @withdraw ; op JVaultUpdateRewards @update }
}

matcher jvault_unstake_request {
    produces JVaultUnstakeRequestBlock
    build    build_jvault_unstake_request
    entry    @request_anchor

    op JVaultUnstakeRequest @request_anchor
      -> maybe ( op JVaultRequestUpdateRewards @request
                   -> { maybe $jvault_referrals ; op JVaultUpdateRewards @update } )
}
