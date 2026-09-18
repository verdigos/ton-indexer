tolk 1.4

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
}
