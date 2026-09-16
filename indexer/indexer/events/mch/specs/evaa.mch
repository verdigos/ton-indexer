# EVAA supply, withdrawal, and liquidation build from captures and parsed bodies.
# EvaaUserOp exposes the opcode after the variable-length user header.
# Exclusive outcome arms bind the captures used by each build.

import "lib/leg.mch"

opcode EvaaSupplySuccess                   = 0x11a
opcode EvaaSupplyFail                      = 0x11f
opcode EvaaWithdrawMaster                  = 0x2
opcode EvaaWithdrawCollateralized          = 0x211
opcode EvaaWithdrawSuccess                 = 0x211a
opcode EvaaWithdrawNoFundsExcess           = 0x211fe8
opcode EvaaWithdrawLockedExcess            = 0x21e6
opcode EvaaWithdrawNotCollateralizedExcess = 0x21e7
opcode EvaaWithdrawMissingPricesExcess     = 0x21e8
opcode EvaaWithdrawExecutionCrashed        = 0x21ec
opcode EvaaLiquidateMaster                 = 0x3
opcode EvaaLiquidateSatisfied              = 0x311
opcode EvaaLiquidateUnsatisfied            = 0x31f
opcode EvaaLiquidateSuccessReport          = 0x311d
opcode EvaaLiquidateFail                   = 0x311f

predicate evaa_supply_anchor
predicate evaa_liquidate_anchor

# EVAA supply starts with a TON call or a jetton transfer carrying a supply
# payload. The user node parses the opcode behind the variable header.
# Outcomes are success with an optional service comment, failure, or refund.
# The build reads the recipient and asset ID from parsed bodies and transfer
# fields from leg(master). Success supplies the final amount; an on-behalf
# refund identifies the recipient's jetton wallet.

matcher evaa_supply {
    produces evaa_supply
    entry    @master

    pred evaa_supply_anchor @master
      -> btype call_contract @user where ((parse? . as EvaaUserOp).op == 0x11)
        -> or {
               op EvaaSupplySuccess @success as EvaaSupplySuccess
                 -> maybe btype ton_transfer where (.comment in ["EVAA liquidation.", "EVAA supply.", "EVAA withdraw."])
               op EvaaSupplyFail @fail
               btype jetton_transfer @refund
           }

    let jetton = master.btype == "jetton_transfer"
    let fwd = (parse? master.data.forward_payload as EvaaSupplyJettonForward) if jetton else null
    let mb = (parse? master as EvaaSupplyMaster) if not jetton else null
    parse user as EvaaSupplyUserHdr
    reject when (jetton and fwd == null) or (not jetton and mb == null) or user.body == null
    reject when success != null and success.body == null
    let l = leg(master)
    let recipient = account(fwd.recipient_address if jetton else mb.recipient_address) ?? addr_none()
    out {
        sender:                  l.source
        recipient:               recipient
        recipient_contract:      user.msg.destination
        amount:                  success.body.amount_supplied if success != null else (l.amount.value if jetton else mb.supply_amount)
        is_success:              success != null
        is_ton:                  not jetton
        asset_id:                user.body.asset_id
        sender_jetton_wallet:    l.source_wallet
        recipient_jetton_wallet: null if not jetton else (l.source_wallet if l.source == recipient else (refund.data.receiver_wallet if refund != null and refund.data.sender_wallet == l.destination_wallet else null))
        master_jetton_wallet:    l.destination_wallet
        master:                  l.destination
        asset:                   l.asset
    }
}

matcher evaa_withdraw {
    produces evaa_withdraw
    entry    @master

    op EvaaWithdrawMaster @master as EvaaWithdrawMaster
      -> btype call_contract @user where ((parse? . as EvaaUserOp).op == 0x21)
      -> or {
             op EvaaWithdrawCollateralized @coll_ok as EvaaWithdrawCollateralized
               -> { xor {
                        btype jetton_transfer @jetton_payout
                        op EvaaWithdrawSuccess @ton_payout
                          -> maybe btype ton_transfer where (.comment in ["EVAA liquidation.", "EVAA supply.", "EVAA withdraw."])
                    }
                    btype call_contract where ((parse? . as EvaaUserOp).op == 0x211a) -> btype ton_transfer }
             op EvaaWithdrawCollateralized @coll_fail
               -> btype call_contract where ((parse? . as EvaaUserOp).op == 0x211f)
               -> op EvaaWithdrawNoFundsExcess
             op [EvaaWithdrawLockedExcess, EvaaWithdrawNotCollateralizedExcess, EvaaWithdrawMissingPricesExcess, EvaaWithdrawExecutionCrashed] @fail_excess as EvaaWithdrawFailExcess
         }

    # Declarative build, field-for-field with build_evaa_withdraw_core.
    # The IR engine binds @coll_ok / @coll_fail / @fail_excess per the winning
    # exclusive branch (only the compiled path mis-bound the shared-head
    # @coll_ok/@coll_fail are not captured), so `coll_ok != null` is the
    # success test.
    # exactly like the core's `if withdraw_collateralized_success:` branch.
    let is_ton  = master.body.asset_id == 0x1A4219FE5E60D63AF2A3CC7DCE6FEC69B45C6B5718497A6148E7C232AC87BD8A
    let success = coll_ok != null
    # Legacy `return None` guards: a collateralized-success outcome must carry the
    # matching payout leg; a match with no outcome group is impossible structurally
    # but the last guard mirrors the core's trailing `return None`.
    reject when success and is_ton and ton_payout == null
    reject when success and not is_ton and jetton_payout == null
    reject when coll_ok == null and coll_fail == null and fail_excess == null
    out {
        owner:                   master.msg.source
        owner_contract:          user.msg.destination
        recipient:               account(master.body.recipient_address)
        master:                  master.msg.destination
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

# The liquidation satisfied arms share 0x311. The success arm parses the user
# header before testing 0x311a; the fail arm tests plain 0x311f.
matcher evaa_liquidate {
    produces evaa_liquidate
    entry    @master

    pred evaa_liquidate_anchor @master
      -> or {
             btype call_contract @user where ((parse? . as EvaaUserOp).op == 0x31)
               -> or {
                      op EvaaLiquidateSatisfied @satisfied_success as EvaaLiquidateSatisfied
                        -> { btype call_contract where ((parse? . as EvaaUserOp).op == 0x311a)
                               -> maybe btype ton_transfer where (.comment in ["EVAA liquidation.", "EVAA supply.", "EVAA withdraw."])
                             xor {
                                 op EvaaLiquidateSuccessReport @finish
                                 op EvaaLiquidateFail @finish
                                 btype jetton_transfer @finish
                             } }
                      op EvaaLiquidateSatisfied @satisfied_fail
                        -> { op EvaaLiquidateFail
                             xor {
                                 op EvaaLiquidateSuccessReport @finish
                                 op EvaaLiquidateFail @finish
                                 btype jetton_transfer @finish
                             } }
                      op EvaaLiquidateUnsatisfied @unsatisfied as EvaaLiquidateUnsatisfied
                        -> xor {
                               op EvaaLiquidateSuccessReport @finish
                               op EvaaLiquidateFail @finish
                               btype jetton_transfer @finish
                           }
                  }
             xor {
                 btype call_contract @immediate where (.msg.bounced)
                 btype jetton_transfer @immediate
             }
         }

    let jetton = master.btype == "jetton_transfer"
    let m = (parse? master.data.forward_payload as EvaaLiquidateJettonForward) if jetton else (parse? master as EvaaLiquidateMaster)
    let success = satisfied_success != null
    let s = satisfied_success.body.info.ref
    let l = leg(master)
    reject when m == null or (success and satisfied_success.body == null)
    out {
        liquidator:          l.source
        borrower:            account(satisfied_success.body.owner_address if success else m.borrower_address) ?? addr_none()
        borrower_contract:   user.msg.destination
        collateral_asset_id: s.collateral_asset_id if success else m.collateral_asset_id
        collateral_amount:   s.collateral_reward if success else 0
        is_success:          success
        fail_reason:         null if success else ("immediate_rejection" if immediate != null else ((unsatisfied.body.reason ?? "liquidation_error") if unsatisfied != null else "master_not_enough_liquidity"))
        debt_amount:         s.liquidatable_amount if success else (l.amount.value if jetton else m.liquidate_incoming_amount)
    }
}
