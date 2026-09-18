tolk 1.4

struct (0b0000) AssetTon {}

struct (0b0001) AssetJetton {
    workchain: int8;
    hash: bits256;
}

type Asset = AssetTon | AssetJetton

struct DedustSwapInfo {
    sender_address: any_address;
    ref_address: any_address;
    reserve_0: coins;
    reserve_1: coins;
}

@overflow1023_policy("suppress")
struct DedustDepositLiquidityAssets {
    asset0: Asset;
    asset0_amount: coins;
    asset1: Asset;
    asset1_amount: coins;
}

@overflow1023_policy("suppress")
struct (0xad4eb6f5) DedustPayoutFromPool {
    query_id: uint64;
    proof: cell;
    amount: coins;
}

@overflow1023_policy("suppress")
struct (0x9c610de3) DedustSwapNotification {
    asset_in: Asset;
    asset_out: Asset;
    amount_in: coins;
    amount_out: coins;
    info: Cell<DedustSwapInfo>;
}

@overflow1023_policy("suppress")
struct (0xb56b9598) DedustDepositLiquidityToPool {
    _query_id: bits64;
    _proof: cell;
    owner: any_address;
    _min_lp: coins;
    field4: Cell<DedustDepositLiquidityAssets>;
}

@overflow1023_policy("suppress")
struct DedustDepositLiquidityParams {
    _min_lp: coins;
    amount0: coins;
    amount1: coins;
}

@overflow1023_policy("suppress")
struct (0xd55e4686) DedustDepositLiquidityNative {
    _query_id: bits64;
    amount: coins;
    _pool_type: bits1;
    asset0: Asset;
    asset1: Asset;
    params: Cell<DedustDepositLiquidityParams>;
}

@overflow1023_policy("suppress")
struct (0x40e108d6) DedustDepositLiquidityJetton {
    _pool_type: bits1;
    asset0: Asset;
    asset1: Asset;
    _min_lp: coins;
    amount0: coins;
    amount1: coins;
}

type AllowedMessage = DedustPayoutFromPool | DedustSwapNotification | DedustDepositLiquidityToPool | DedustDepositLiquidityNative | DedustDepositLiquidityJetton

contract DedustMessages {
    incomingMessages: AllowedMessage
}
