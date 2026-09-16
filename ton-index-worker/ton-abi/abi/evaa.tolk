tolk 1.4

@overflow1023_policy("suppress")
struct (0x00000001) EvaaSupplyMaster {
    _query_id: bits64;
    _include_user_code: bits2;
    supply_amount: uint64;
    recipient_address: any_address;
}

@overflow1023_policy("suppress")
struct (0x0000011a) EvaaSupplySuccess {
    _query_id: bits64;
    _owner_address: any_address;
    _asset_id: uint256;
    amount_supplied: uint64;
}

@overflow1023_policy("suppress")
struct (0x00000002) EvaaWithdrawMaster {
    query_id: uint64;
    asset_id: uint256;
    amount: uint64;
    recipient_address: any_address;
}

@overflow1023_policy("suppress")
struct (0x00000211) EvaaWithdrawCollateralized {
    query_id: uint64;
    owner_address: any_address;
    asset_id: uint256;
    withdraw_amount_current: uint64;
}

struct (0x000021e6) EvaaWithdrawLockedExcess {}

struct (0x000021e7) EvaaWithdrawNotCollateralizedExcess {}

struct (0x000021e8) EvaaWithdrawMissingPricesExcess {}

struct (0x000021ec) EvaaWithdrawExecutionCrashed {}

@overflow1023_policy("suppress")
struct (0x00000003) EvaaLiquidateMaster {
    _query_id: bits64;
    borrower_address: any_address;
    collateral_asset_id: uint256;
    _min_collateral_amount: bits64;
    _include_user_code: bits2;
    liquidate_incoming_amount: uint64;
}

struct EvaaLiquidateSatisfiedInfo {
    _delta_loan_principal: int64;
    liquidatable_amount: uint64;
    _protocol_gift: uint64;
    _new_user_loan_principal: int64;
    collateral_asset_id: uint256;
    _delta_collateral_principal: int64;
    collateral_reward: uint64;
}

@overflow1023_policy("suppress")
struct (0x00000311) EvaaLiquidateSatisfied {
    _query_id: bits64;
    owner_address: any_address;
    _liquidator_address: any_address;
    _transferred_asset_id: bits256;
    info: Cell<EvaaLiquidateSatisfiedInfo>;
}

struct EvaaLiquidateUnsatisfiedInfo {
    _transferred_amount: uint64;
    _collateral_asset_id: uint256;
    _min_collateral_amount: uint64;
    _custom_response_payload: cell;
    _subaccount_id: int16;
    error: uint32;
    _error_query_id: bits64;
}

@overflow1023_policy("suppress")
struct (0x0000031f) EvaaLiquidateUnsatisfied {
    _query_id: bits64;
    _owner_address: any_address;
    _liquidator_address: any_address;
    _transferred_asset_id: bits256;
    info: Cell<EvaaLiquidateUnsatisfiedInfo>;
}

type AllowedMessage = EvaaSupplyMaster | EvaaSupplySuccess | EvaaWithdrawMaster | EvaaWithdrawCollateralized | EvaaWithdrawLockedExcess | EvaaWithdrawNotCollateralizedExcess | EvaaWithdrawMissingPricesExcess | EvaaWithdrawExecutionCrashed | EvaaLiquidateMaster | EvaaLiquidateSatisfied | EvaaLiquidateUnsatisfied

contract EvaaMessages {
    incomingMessages: AllowedMessage
}
