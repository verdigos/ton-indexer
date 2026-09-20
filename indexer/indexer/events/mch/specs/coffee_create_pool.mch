# Coffee pool creation covers both funding legs, the deployment of a creator for each user, and the final pool deployment.

import "lib/opcodes.mch"
opcode CoffeeCreatePoolNative                = 0xC0FFEE02
opcode CoffeeCreatePoolCreatorRequest        = 0xC0FFEE23
opcode CoffeeCreatePoolRequest               = 0xC0FFEE25
opcode CoffeeNotificationCallback            = 0xB216D31F
matcher coffee_create_pool_creator {
    produces coffee_create_pool_creator

    op CoffeeCreatePoolCreatorRequest @request as CoffeeCreatePoolCreatorRequest {
        -> { op CoffeeDeploy @deploy }
        <- xor {
               btype jetton_transfer @in_jt
               op CoffeeCreatePoolNative
           }
    }

    let pool_first  = asset_of(request.body.pool_first)
    let pool_second = asset_of(request.body.pool_second)
    out {
        sender:                account(request.body.sender)
        sender_jetton_wallet:  in_jt.data.sender_wallet ?? addr_none()
        amount:                amount(request.body.amount)
        provided_asset:        in_jt.data.asset ?? ton_asset()
        pool_creator_contract: deploy.msg.destination
        deposit_recipient:     account(request.body.creation_params.ref.recipient)
        pool_first:            pool_first
        pool_second:           pool_second
    }
}

matcher coffee_create_pool {
    produces coffee_create_pool

    btype coffee_create_pool_creator @creator
      -> op CoffeeCreatePoolRequest @request as CoffeeCreatePoolRequest
        -> op CoffeeDeploy @deploy
          -> { op JettonInternalTransfer -> maybe op Excess
             ; op CoffeeDepositLiquiditySuccessfulEvent @success as CoffeeDepositLiquiditySuccessfulEvent
             ; maybe op CoffeeNotification -> maybe op CoffeeNotificationCallback }


    let provided  = creator.data.provided_asset
    let pool_1    = creator.data.pool_first
    let pool_2    = creator.data.pool_second
    let is_first  = provided == pool_1
    let is_second = provided == pool_2
    let initiator = account(request.body.tx_initiator)
    let a_1       = amount(request.body.amount1 ?? 0)
    let a_2       = amount(request.body.amount2 ?? 0)
    out {
        source:                creator.data.sender
        source_jetton_wallet:  creator.data.sender_jetton_wallet
        provided_asset:        provided
        amount:                creator.data.amount
        initiator_1:           initiator if is_second else creator.data.sender
        initiator_2:           creator.data.sender if is_second else initiator
        pool:                  deploy.msg.destination
        asset_1:               provided if (is_first or is_second) else pool_1
        asset_2:               pool_1 if is_second else pool_2
        amount_1:              a_2 if is_second else a_1
        amount_2:              a_1 if is_second else a_2
        lp_tokens_minted:      amount(success.body.lp_amount ?? 0)
        pool_creator_contract: creator.data.pool_creator_contract
    }
}
