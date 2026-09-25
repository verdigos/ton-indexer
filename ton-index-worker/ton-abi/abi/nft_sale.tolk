tolk 1.4

@overflow1023_policy("suppress")
struct SaleFeesV1 {
    marketplace_fee: coins;
    marketplace_fee_address: any_address;
    royalty_address: any_address;
    royalty_amount: coins;
}

@overflow1023_policy("suppress")
struct SaleFeesLegacy {
    marketplace_fee_address: any_address;
    marketplace_fee: coins;
    royalty_address: any_address;
    royalty_amount: coins;
}

@overflow1023_policy("suppress")
struct SaleStatic {
    marketplace_fee_address: any_address;
    royalty_address: any_address;
    _marketplace_fee_percent: bits17;
    _royalty_percent: bits17;
}

@overflow1023_policy("suppress")
struct SaleV1 {
    marketplace_address: any_address;
    _nft_address: any_address;
    _nft_owner_address: any_address;
    full_price: coins;
    fees: Cell<SaleFeesV1>;
}

@overflow1023_policy("suppress")
struct SaleLegacy {
    _is_complete: bits1;
    _created_at: bits32;
    marketplace_address: any_address;
    _nft_address: any_address;
    _nft_owner_address: any_address;
    full_price: coins;
    fees: Cell<SaleFeesLegacy>;
}

@overflow1023_policy("suppress")
struct SaleV4r1 {
    _is_complete: bits1;
    marketplace_address: any_address;
    _nft_owner_address: any_address;
    full_price: coins;
    _sold_at: bits32;
    _query_id: bits64;
    fees: Cell<SaleStatic>;
}

@overflow1023_policy("suppress")
struct AuctionFees {
    mp_fee_addr: any_address;
    mp_fee_factor: uint32;
    mp_fee_base: uint32;
    royalty_fee_addr: any_address;
    _royalty_fee_factor: bits32;
    royalty_fee_base: uint32;
}

struct AuctionBids {
    min_bid: coins;
    max_bid: coins;
}

struct AuctionConstantV3r2 {
    _sub_gas_price_from_bid: bits32;
    _mp_addr: any_address;
    min_bid: coins;
    max_bid: coins;
}

@overflow1023_policy("suppress")
struct AuctionFeesV3r3 {
    mp_fee_addr: any_address;
    royalty_fee_addr: any_address;
}

struct AuctionConstantV3r3 {
    _mp_addr: any_address;
    min_bid: coins;
    max_bid: coins;
}

@overflow1023_policy("suppress")
struct AuctionFeesV4r1 {
    mp_fee_addr: any_address;
    royalty_fee_addr: any_address;
    mp_fee_factor: uint32;
    mp_fee_base: uint32;
    _royalty_fee_factor: bits32;
    royalty_fee_base: uint32;
}

struct AuctionConstantV4r1 {
    _mp_addr: any_address;
    min_bid: coins;
    max_bid: coins;
}

struct AuctionV1 {
    fees: Cell<AuctionFees>;
    constant: Cell<AuctionBids>;
}

struct AuctionV3r2 {
    _end: bits1;
    _activated: bits1;
    _is_canceled: bits1;
    _last_member: any_address;
    _last_bid: coins;
    _last_bid_at: bits32;
    _end_time: bits32;
    fees: Cell<AuctionFees>;
    constant: Cell<AuctionConstantV3r2>;
}

@overflow1023_policy("suppress")
struct AuctionV3r3 {
    _end: bits1;
    _is_canceled: bits1;
    _last_member: any_address;
    _last_bid: coins;
    _last_bid_at: bits32;
    _end_time: bits32;
    _nft_owner: any_address;
    _last_query_id: bits64;
    mp_fee_factor: uint32;
    mp_fee_base: uint32;
    _royalty_fee_factor: bits32;
    royalty_fee_base: uint32;
    fees: Cell<AuctionFeesV3r3>;
    constant: Cell<AuctionConstantV3r3>;
}

@overflow1023_policy("suppress")
struct AuctionV4r1 {
    _end: bits1;
    _is_canceled: bits1;
    _last_member: any_address;
    _last_bid: coins;
    _last_bid_at: bits32;
    _end_time: bits32;
    _nft_owner: any_address;
    _last_query_id: bits64;
    fees: Cell<AuctionFeesV4r1>;
    constant: Cell<AuctionConstantV4r1>;
}

@overflow1023_policy("suppress")
struct (0x05138d91) NftOwnershipAssignedPrevOwner {
    _query_id: bits64;
    prev_owner: any_address;
}

@overflow1023_policy("suppress")
struct (0x5fcc3d14) NftTransfer {
    query_id: uint64;
    new_owner: any_address;
    response_destination: any_address;
    custom_payload: cell?;
    forward_amount: coins;
}

@overflow1023_policy("suppress")
struct (0x6c6c2080) SaleUpdateMessage {
    _query_id: bits64;
    new_full_price: coins;
    new_mp_fee: coins;
    new_royalty_amount: coins;
}

@overflow1023_policy("suppress")
struct (0x8b771735) NftReportStaticData {
    query_id: uint64;
    index: uint256;
    collection: any_address;
}

type AllowedMessage = NftOwnershipAssignedPrevOwner | NftTransfer | SaleUpdateMessage | NftReportStaticData

contract NftSaleMessages {
    incomingMessages: AllowedMessage
    forceAbiExport: SaleV1 | SaleLegacy | SaleV4r1 | AuctionV1 | AuctionV3r2 | AuctionV3r3 | AuctionV4r1
}
