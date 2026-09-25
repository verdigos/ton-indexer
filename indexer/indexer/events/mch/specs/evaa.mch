# Matches EVAA native and jetton supplies, collateral withdrawals, and liquidations.

import "lib/leg.mch"

opcode EvaaSupplyMaster                    = 0x1
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

rule evaa_note = btype ton_transfer where (.comment in ["EVAA liquidation.", "EVAA supply.", "EVAA withdraw."])
rule liq_finish = xor { op EvaaLiquidateSuccessReport; op EvaaLiquidateFail; btype jetton_transfer }
fn evaa_op(b) = (parse? b as EvaaUserOp).op
fn evaa_refund_reason(code) = (
    "liquidate_asset_transaction_fees" if code == 0x30e9 else
    "master_liquidating_too_much" if code == 0x30f1 else
    "user_withdraw_in_progress" if code == 0x31f0 else
    "not_liquidatable" if code == 0x31f2 else
    "min_collateral_not_satisfied" if code == 0x31f3 else
    "user_not_enough_collateral" if code == 0x31f4 else
    "user_liquidating_too_much" if code == 0x31f5 else
    "master_not_enough_liquidity" if code == 0x31f6 else
    "liquidation_prices_missing" if code == 0x31f7 else
    "liquidation_execution_crashed" if code == 0x31fe else null
)
fn evaa_liquidate_fail(immediate, refund, unsatisfied) = (
    "immediate_rejection" if immediate != null else
    evaa_refund_reason(refund.msg.opcode) if refund != null else
    (evaa_refund_reason(unsatisfied.body.info.ref.error) ?? "unknown") if unsatisfied != null else
    "master_not_enough_liquidity"
)
fn evaa_withdraw_reason(code) = (
    "withdraw_locked_excess" if code == 0x21e6 else
    "withdraw_not_collateralized_excess" if code == 0x21e7 else
    "withdraw_missing_prices_excess" if code == 0x21e8 else
    "withdraw_execution_crashed" if code == 0x21ec else null
)

rule evaa_supply_flow =
    btype call_contract @user where (evaa_op(.) == 0x11)
      -> or {
             op EvaaSupplySuccess @success as EvaaSupplySuccess
               -> maybe $evaa_note
             op EvaaSupplyFail
             btype jetton_transfer @refund
         }

rule evaa_liquidate_flow =
    or {
        btype call_contract @user where (evaa_op(.) == 0x31)
          -> or {
                 op EvaaLiquidateSatisfied @satisfied_success as EvaaLiquidateSatisfied
                   -> { btype call_contract where (evaa_op(.) == 0x311a)
                          -> maybe $evaa_note
                        $liq_finish }
                 op EvaaLiquidateSatisfied
                   -> { op EvaaLiquidateFail
                        $liq_finish }
                 op EvaaLiquidateUnsatisfied @unsatisfied as EvaaLiquidateUnsatisfied
                   -> $liq_finish
             }
        xor {
            btype call_contract @immediate where (.msg.bounced)
            btype jetton_transfer @immediate
        }
    }

# EVAA refunds carry the error code as their opcode.
rule evaa_liquidate_refund_leg =
    btype call_contract @refund where (evaa_refund_reason(.msg.opcode) != null)

rule evaa_liquidate_refund =
    or {
        btype call_contract @user where (evaa_op(.) == 0x31)
          -> op [EvaaLiquidateSatisfied, EvaaLiquidateUnsatisfied]
          -> $evaa_liquidate_refund_leg
        $evaa_liquidate_refund_leg
    }

matcher evaa_supply {
    produces evaa_supply

    op EvaaSupplyMaster @master -> $evaa_supply_flow

    let mb = parse? master as EvaaSupplyMaster
    parse user as EvaaSupplyUserHdr
    reject when mb == null or user.body == null
    reject when success != null and success.body == null
    let l = leg(master)
    let recipient = account(mb.recipient_address) ?? addr_none()
    out {
        sender:                  l.source
        recipient:               recipient
        recipient_contract:      user.msg.destination
        amount:                  success.body.amount_supplied if success != null else mb.supply_amount
        is_success:              success != null
        is_ton:                  true
        asset_id:                user.body.asset_id
        sender_jetton_wallet:    l.source_wallet
        recipient_jetton_wallet: null
        master_jetton_wallet:    l.destination_wallet
        master:                  l.destination
        asset:                   l.asset
    }
}

matcher evaa_supply_jetton {
    produces evaa_supply

    btype jetton_transfer @master where (.payload_opcode == 0x1) -> $evaa_supply_flow

    let fwd = parse? master.data.forward_payload as EvaaSupplyJettonForward
    parse user as EvaaSupplyUserHdr
    reject when fwd == null or user.body == null
    reject when success != null and success.body == null
    let l = leg(master)
    let recipient = account(fwd.recipient_address) ?? addr_none()
    out {
        sender:                  l.source
        recipient:               recipient
        recipient_contract:      user.msg.destination
        amount:                  success.body.amount_supplied if success != null else l.amount.value
        is_success:              success != null
        is_ton:                  false
        asset_id:                user.body.asset_id
        sender_jetton_wallet:    l.source_wallet
        recipient_jetton_wallet: l.source_wallet if l.source == recipient else (refund.data.receiver_wallet if refund != null and refund.data.sender_wallet == l.destination_wallet else null)
        master_jetton_wallet:    l.destination_wallet
        master:                  l.destination
        asset:                   l.asset
    }
}

matcher evaa_withdraw {
    produces evaa_withdraw

    op EvaaWithdrawMaster @master as EvaaWithdrawMaster
      -> btype call_contract @user where (evaa_op(.) == 0x21)
      -> or {
             op EvaaWithdrawCollateralized @coll_ok as EvaaWithdrawCollateralized
               -> { xor {
                        btype jetton_transfer @jetton_payout
                        op EvaaWithdrawSuccess @ton_payout
                          -> maybe $evaa_note
                    }
                    btype call_contract where (evaa_op(.) == 0x211a) -> btype ton_transfer }
             op EvaaWithdrawCollateralized @coll_fail
               -> btype call_contract where (evaa_op(.) == 0x211f)
               -> op EvaaWithdrawNoFundsExcess
             op [EvaaWithdrawLockedExcess, EvaaWithdrawNotCollateralizedExcess, EvaaWithdrawMissingPricesExcess, EvaaWithdrawExecutionCrashed] @fail_excess
         }

    let is_ton  = master.body.asset_id == 0x1A4219FE5E60D63AF2A3CC7DCE6FEC69B45C6B5718497A6148E7C232AC87BD8A
    let success = coll_ok != null
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
        fail_reason:             null if success else ("withdraw_no_funds_excess" if coll_fail != null else evaa_withdraw_reason(fail_excess.msg.opcode))
    }
}

matcher evaa_liquidate {
    produces evaa_liquidate

    btype jetton_transfer @master where (.payload_opcode == 0x3)
      -> $evaa_liquidate_flow

    let m = parse? master.data.forward_payload as EvaaLiquidateJettonForward
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
        fail_reason:         null if success else evaa_liquidate_fail(immediate, null, unsatisfied)
        debt_amount:         s.liquidatable_amount if success else l.amount.value
    }
}

matcher evaa_liquidate_ton {
    produces evaa_liquidate

    op EvaaLiquidateMaster @master as EvaaLiquidateMaster
      -> or { $evaa_liquidate_flow; $evaa_liquidate_refund }

    let success = satisfied_success != null
    let s = satisfied_success.body.info.ref
    let v7 = parse? master as EvaaLiquidateMasterV7
    # A parsed v7 body is accepted only with zero trailing inline bits and a matching sender.
    reject when v7 != null and account(v7.liquidator_address) != master.msg.source
    reject when v7 == null and master.body == null
    reject when success and satisfied_success.body == null
    let m = v7 if v7 != null else master.body
    # EvaaLiquidateMaster carries borrower_address for both outcomes; the ABI does not
    # equate it with the success reply's owner_address.
    out {
        liquidator:          master.msg.source
        borrower:            account(m.borrower_address) ?? addr_none()
        borrower_contract:   user.msg.destination
        collateral_asset_id: s.collateral_asset_id if success else m.collateral_asset_id
        collateral_amount:   s.collateral_reward if success else 0
        is_success:          success
        fail_reason:         null if success else evaa_liquidate_fail(immediate, refund, unsatisfied)
        debt_amount:         s.liquidatable_amount if success else m.liquidate_incoming_amount
    }
}
