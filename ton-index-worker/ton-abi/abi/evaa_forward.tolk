tolk 1.4

@overflow1023_policy("suppress")
struct (0x00000001) EvaaSupplyJettonForward {
    _include_user_code: bits2;
    recipient_address: any_address;
}

@overflow1023_policy("suppress")
struct (0x00000003) EvaaLiquidateJettonForward {
    borrower_address: any_address;
    collateral_asset_id: uint256;
    _min_collateral_amount: bits64;
    _include_user_code: bits2;
    liquidate_incoming_amount: uint64;
}

type AllowedMessage = EvaaSupplyJettonForward | EvaaLiquidateJettonForward

contract EvaaForwardMessages {
    incomingMessages: AllowedMessage
}
