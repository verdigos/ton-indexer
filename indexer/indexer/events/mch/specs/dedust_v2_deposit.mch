# DeDust V2 deposit flow has final, rejected, and partial matches; a rejected join with no refund leg belongs outside this matcher.

import "lib/leg.mch"
import "lib/opcodes.mch"

opcode DedustV2CreditAsset     = 0xdc5ddba1
opcode DedustV2JoinLiquidity   = 0x25251ee0
opcode DedustV2DepositEvent    = 0x35df2e12
opcode DedustV2CreditLiquidity = 0x855afcbd
rule dv2_deposit_carrier = xor {
                               op DedustV2PayNative @carrier
                               btype jetton_transfer @carrier
                           }

rule dv2_credit_parent =
    op DedustV2CreditAsset @credit <- $dv2_deposit_carrier

matcher dedust_v2_deposit_final {
    produces        dedust_v2_deposit_liquidity
    include_excess  false

    op DedustV2JoinLiquidity @join as DedustV2JoinLiquidity {
        -> {
            maybe op DedustV2DepositEvent @event as DedustV2DepositEvent
            ; maybe op DedustV2CreditLiquidity
            ; maybe btype jetton_transfer @refund_jt_1
            ; maybe btype jetton_transfer @refund_jt_2
            ; maybe op PayoutMessage @refund_ton
        }
        <- $dv2_credit_parent
    }

    parse credit     as DedustV2CreditAsset
    parse carrier    as DedustV2PayNative
    parse refund_ton as PayoutMessage

    reject when event == null

    let initiator = account(join.body.initiator)
    let is_x      = credit.body.is_x
    let jetton_in = carrier.btype == "jetton_transfer"

    let l = leg(carrier)

    let carrier_cell = (parse? carrier.data.forward_payload as DedustV2PayJetton).payment_payload if jetton_in else carrier.body.payment_payload
    let payload      = parse? carrier_cell as DedustV2DepositPayload

    reject when refund_jt_1 != null and refund_jt_1.data.receiver != initiator
    reject when refund_jt_2 != null and refund_jt_2.data.receiver != initiator
    reject when refund_ton != null and refund_ton.msg.destination != initiator

    let jetton_excesses = concat(
        [[refund_jt_1.data.asset, refund_jt_1.data.amount]] if refund_jt_1 != null else [],
        [[refund_jt_2.data.asset, refund_jt_2.data.amount]] if refund_jt_2 != null else [])
    let ton_excesses    = [[ton_asset(), amount(refund_ton.body.amount)]] if refund_ton != null else []

    out {
        dex:              "dedust_v2"
        sender:           initiator
        pool:             join.msg.destination
        deposit_contract: join.msg.source
        amount_1:         amount(event.body.amount_x_in)
        asset_1:          l.asset if is_x else null
        sender_wallet_1:  l.source_wallet if is_x else null
        amount_2:         amount(event.body.amount_y_in)
        asset_2:          null if is_x else l.asset
        sender_wallet_2:  null if is_x else l.source_wallet
        target_amount_1:  amount(payload.amount_x)
        target_amount_2:  amount(payload.amount_y)
        lp_tokens_minted: amount(event.body.liquidity)
        vault_excesses:   concat(jetton_excesses, ton_excesses)
    }
}

matcher dedust_v2_deposit_rejected {
    produces        dedust_v2_deposit_liquidity
    include_excess  false

    op DedustV2JoinLiquidity @join as DedustV2JoinLiquidity {
        -> {
            maybe btype jetton_transfer @refund_jt_1
            ; maybe btype jetton_transfer @refund_jt_2
            ; maybe op PayoutMessage @refund_ton
            ; maybe op DedustV2DepositEvent @event
            ; maybe op Excess
        }
        <- $dv2_credit_parent
    }

    parse credit     as DedustV2CreditAsset
    parse carrier    as DedustV2PayNative
    parse refund_ton as PayoutMessage

    reject when event != null

    let initiator = account(join.body.initiator)
    let is_x      = credit.body.is_x
    let jetton_in = carrier.btype == "jetton_transfer"

    let l = leg(carrier)

    let carrier_cell = (parse? carrier.data.forward_payload as DedustV2PayJetton).payment_payload if jetton_in else carrier.body.payment_payload
    let payload      = parse? carrier_cell as DedustV2DepositPayload

    reject when refund_jt_1 != null and refund_jt_1.data.receiver != initiator
    reject when refund_jt_2 != null and refund_jt_2.data.receiver != initiator
    reject when refund_ton != null and refund_ton.msg.destination != initiator

    let jetton_excesses = concat(
        [[refund_jt_1.data.asset, refund_jt_1.data.amount]] if refund_jt_1 != null else [],
        [[refund_jt_2.data.asset, refund_jt_2.data.amount]] if refund_jt_2 != null else [])
    let ton_excesses    = [[ton_asset(), amount(refund_ton.body.amount)]] if refund_ton != null else []
    let excesses        = concat(jetton_excesses, ton_excesses)
    reject when len(excesses) == 0

    failed when true

    out {
        dex:              "dedust_v2"
        sender:           initiator
        pool:             join.msg.destination
        deposit_contract: join.msg.source
        amount_1:         amount(join.body.amount_x)
        asset_1:          l.asset if is_x else null
        sender_wallet_1:  l.source_wallet if is_x else null
        amount_2:         amount(join.body.amount_y)
        asset_2:          null if is_x else l.asset
        sender_wallet_2:  null if is_x else l.source_wallet
        target_amount_1:  amount(payload.amount_x)
        target_amount_2:  amount(payload.amount_y)
        lp_tokens_minted: null
        vault_excesses:   excesses
    }
}

matcher dedust_v2_deposit_partial {
    produces        dedust_v2_deposit_liquidity_partial
    include_excess  false

    op DedustV2CreditAsset @credit as DedustV2CreditAsset {
        -> { maybe op Excess }
        <- $dv2_deposit_carrier
    }

    parse carrier as DedustV2PayNative

    let is_x      = credit.body.is_x

    let l = leg(carrier)
    let leg_amount = amount(credit.body.amount)

    out {
        dex:              "dedust_v2"
        sender:           l.source
        pool:             credit.msg.source
        deposit_contract: credit.msg.destination
        amount_1:         leg_amount if is_x else null
        asset_1:          l.asset if is_x else null
        sender_wallet_1:  l.source_wallet if is_x else null
        amount_2:         null if is_x else leg_amount
        asset_2:          null if is_x else l.asset
        sender_wallet_2:  null if is_x else l.source_wallet
        lp_tokens_minted: null
    }
}
