# EVAA withdraw — full port of blocks/evaa.py EvaaWithdrawBlockMatcher.
#
# Deviations from the legacy matcher (see the parity notes in
# tests/mch/integration/test_evaa_withdraw.py):
# - EVAA user-contract messages prefix the 32-bit opcode with a variable-length
#   header (version coins + maybe upgrade-info ref + 2 bits), so a plain `op`
#   node cannot see the opcode. Legacy uses EvaaContractWithHeaderMatcher
#   (test_self override); here those three nodes are `pred` nodes backed by
#   host predicates (evaa_user_withdraw_*) with exactly the legacy test logic.
#   M4 inline predicates cannot replace them (body parsing, not a data field).
# - The EVAA service-comment ton_transfer (legacy evaa_action_comment_matcher,
#   a GenericMatcher over TonTransferBlock.comment) is `maybe pred
#   evaa_service_comment` — set membership over a block attribute is not
#   expressible as an M4 inline field test either.
# - Legacy's withdraw_fail_on_user_excesses GenericMatcher (opcode-set
#   membership over 4 error opcodes) is an exclusive alternative of the four
#   concrete `op` nodes sharing one capture (@fail_excess); same block matched,
#   same consumed set, and only the winning branch binds (spec §6.2).
# - `^` mirrors legacy's ExclusiveOrMatcher use. As an edge target the
#   exclusivity gate is try_build-inert (engine-v2 §4.4), so branch order is
#   first-match-wins exactly like legacy.
#
# Capture -> legacy label map:
#   @master        (anchor)                      | @user      withdraw_on_user
#   @coll_ok       withdraw_collateralized_success
#   @jetton_payout jetton_payout                 | @ton_payout ton_payout
#   @coll_fail     withdraw_collateralized_fail
#   @fail_excess   withdraw_fail_on_user_excesses
# (withdraw_success / withdraw_fail_no_funds / withdraw_no_funds_excess are
# structural-only in the legacy build — no captures needed.)
#
# Captures are kept for pattern fidelity but the builder works off the
# consumed set (Match._mch_consumed): the compiled path's positional capture
# walker picks an alternative branch by its HEAD test, and @coll_ok/@coll_fail
# share the head `op EvaaWithdrawCollateralized`, so on fail traces it binds
# the wrong one (the v2 IR engine binds during the walk and is unaffected).
# Same situation as stonfi_v2_swap.mch / dedust_swap.mch.

opcode EvaaWithdrawMaster                  = 0x2
opcode EvaaWithdrawCollateralized          = 0x211
opcode EvaaWithdrawSuccess                 = 0x211a
opcode EvaaWithdrawNoFundsExcess           = 0x211fe8
opcode EvaaWithdrawLockedExcess            = 0x21e6
opcode EvaaWithdrawNotCollateralizedExcess = 0x21e7
opcode EvaaWithdrawMissingPricesExcess     = 0x21e8
opcode EvaaWithdrawExecutionCrashed        = 0x21ec

predicate evaa_user_withdraw_user
predicate evaa_user_withdraw_success
predicate evaa_user_withdraw_fail
predicate evaa_service_comment

matcher evaa_withdraw {
    produces EvaaWithdrawBlock
    entry    @master

    op EvaaWithdrawMaster @master
      -> pred evaa_user_withdraw_user @user
      -> ( op EvaaWithdrawCollateralized @coll_ok
             -> { ( btype jetton_transfer @jetton_payout
                  ^ op EvaaWithdrawSuccess @ton_payout -> maybe pred evaa_service_comment )
                  pred evaa_user_withdraw_success -> btype ton_transfer }
         ^ op EvaaWithdrawCollateralized @coll_fail
             -> pred evaa_user_withdraw_fail
             -> op EvaaWithdrawNoFundsExcess
         ^ ( op EvaaWithdrawLockedExcess @fail_excess
           ^ op EvaaWithdrawNotCollateralizedExcess @fail_excess
           ^ op EvaaWithdrawMissingPricesExcess @fail_excess
           ^ op EvaaWithdrawExecutionCrashed @fail_excess ) )

    # Declarative build (M4 §9), field-for-field with build_evaa_withdraw_core.
    # The IR engine binds @coll_ok / @coll_fail / @fail_excess per the winning
    # exclusive branch (only the compiled path mis-bound the shared-head
    # @coll_ok/@coll_fail — retired), so `coll_ok != null` is the success test
    # exactly like the core's `if withdraw_collateralized_success:` branch.
    parse master     as EvaaWithdrawMaster
    parse coll_ok    as EvaaWithdrawCollateralized     # null on the fail branches
    parse fail_excess as EvaaWithdrawFailExcess         # null unless a user-contract error fired
    let is_ton  = master.body.asset_id == 0x1A4219FE5E60D63AF2A3CC7DCE6FEC69B45C6B5718497A6148E7C232AC87BD8A
    let success = coll_ok != null
    # Legacy `return None` guards: a collateralized-success outcome must carry the
    # matching payout leg; a match with no outcome group is impossible structurally
    # but the last guard mirrors the core's trailing `return None`.
    reject when success and is_ton and ton_payout == null
    reject when success and not is_ton and jetton_payout == null
    reject when coll_ok == null and coll_fail == null and fail_excess == null
    out {
        owner:                   account(master.msg.source)
        owner_contract:          account(user.msg.destination)
        recipient:               account(master.body.recipient_address)
        master:                  account(master.msg.destination)
        asset_id:                master.body.asset_id
        is_ton:                  is_ton
        is_success:              success
        amount:                  coll_ok.body.withdraw_amount_current if success else master.body.amount
        asset:                   (ton_asset() if is_ton else jetton_payout.data.asset) if success else null
        recipient_jetton_wallet: account(jetton_payout.data.receiver_wallet) if (success and not is_ton) else null
        master_jetton_wallet:    account(jetton_payout.data.sender_wallet) if (success and not is_ton) else null
        fail_reason:             null if success else ("withdraw_no_funds_excess" if coll_fail != null else fail_excess.body.reason)
    }
}
