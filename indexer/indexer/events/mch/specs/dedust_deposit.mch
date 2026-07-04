# Dedust two-branch liquidity deposit — ported from blocks/liquidity.py
# DedustDepositBlockMatcher (final) + DedustDepositFirstAssetBlockMatcher
# (partial). Registration order inside this file mirrors event_processing:
# final first, partial second.
#
# Key deviation from the hand-written pair, forced by registration order: the
# legacy matchers run BEFORE JettonTransferBlockMatcher, so they match the
# user's deposit leg as a RAW JettonTransfer -> JettonInternalTransfer ->
# JettonNotify chain and build the vault's jetton-excess payouts themselves
# (labeled(..., JettonTransferBlockMatcher, call_build_block=True)). Here
# jetton_transfer has already run, so the deposit leg and the excess payouts
# arrive as prebuilt `btype jetton_transfer` blocks; the raw notify chain is
# kept as a fallback arm for legs the jetton matcher rejected. Consumed
# event-node sets are identical either way (the composite carries the same
# nodes, including per-node excess absorption).
#
# The spec doc's §14.3 worked example is idealized — its opcode values and the
# rejection/destroy shapes do not match the code; this file follows the code.
#
# Paired partial+final blocks are merged by host-side
# post_process_dedust_liquidity (spec §5.5): both builders return the exact
# registered block classes the pass dispatches on
# (DedustDepositLiquidity / DedustDepositLiquidityPartial).
#
# The partial matcher's legacy guard (raise when final-deposit opcodes follow
# the topup — a failed-final leftover) lives in the shared build core and
# surfaces as a builder reject here.
#
# Flat parent chain `<- op Ask -> { maybe op Deploy } <- $deposit_leg`: ask gets
# BOTH children_matchers=[deploy] and parent=Or(deposit legs), byte-matching the
# legacy structure. (Previously the trailing `<- $leg` parent edge after the
# children block was silently dropped and the hop had to be grouped
# `<- ( op Ask -> {...} <- $leg )`; T04 fixed the lowering so the flat form
# wires identically to the grouped form.)
#
# Builders are consumed-set/tree-driven (Match._mch_consumed + previous_block
# navigation), same approach as dedust_swap/stonfi_v2; captures are kept for
# pattern fidelity only.

opcode JettonTransfer         = 0x0f8a7ea5
opcode JettonInternalTransfer = 0x178d4519
opcode JettonNotify           = 0x7362d09c

opcode DedustDepositTONToVault               = 0xd55e4686
opcode DedustAskLiquidityFactory             = 0xf04ec526
opcode DedustDeployDepositContract           = 0x9b3aa3fa
opcode DedustTopUpLiquidityDepositContract   = 0x54240fe5
opcode DedustDepositLiquidityToPool          = 0xb56b9598
opcode DedustDestroyLiquidityDepositContract = 0xaae79256
opcode DedustReturnExcessFromVault           = 0x6b0b787f
opcode DedustPayout                          = 0x474f86cf
opcode DedustRejection                       = 0xe1a36cd4

# The deposit leg feeding the vault: TON straight to the vault, a prebuilt
# jetton transfer, or the raw notify chain (fallback, see header).
rule deposit_leg = (
    op DedustDepositTONToVault @deposit_ton
  | btype jetton_transfer @deposit_transfer
  | op JettonNotify @deposit_notify <- op JettonInternalTransfer <- op JettonTransfer
)

# One excess-liquidity refund from a vault: optional jetton payout (prebuilt
# transfer) and/or optional TON payout. Legacy: ContractMatcher(optional,
# ReturnExcessFromVault, children=[jetton_excess(optional), ton_excess(optional)]).
rule vault_excess =
    maybe op DedustReturnExcessFromVault
      -> { maybe btype jetton_transfer
         ; maybe op DedustPayout }

matcher dedust_deposit_final {
    produces DedustDepositLiquidity
    build    build_dedust_deposit_final
    entry    @pool

    op DedustDepositLiquidityToPool @pool
      -> { ( op JettonInternalTransfer @lp_transfer -> maybe op JettonNotify
           | op DedustRejection @rejection
               -> { maybe btype ton_transfer ; $vault_excess ; $vault_excess } )
         ; maybe op DedustDestroyLiquidityDepositContract
             -> { maybe btype ton_transfer ; $vault_excess ; $vault_excess } }
      <- op DedustTopUpLiquidityDepositContract
      <- op DedustAskLiquidityFactory
         -> { maybe op DedustDeployDepositContract }
         <- $deposit_leg
}

matcher dedust_deposit_partial {
    produces DedustDepositLiquidityPartial
    build    build_dedust_deposit_partial
    entry    @topup

    op DedustTopUpLiquidityDepositContract @topup
      <- op DedustAskLiquidityFactory
         -> { op DedustDeployDepositContract }
         <- $deposit_leg
}
