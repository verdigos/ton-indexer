# Matches NFT mints from transactions that activate exactly one NFT item account.
# Frozen to active counts as a deployment here, unlike contract_deploy blocks.

predicate single_contract_deploy

matcher nft_mint {
    produces  nft_mint

    pred single_contract_deploy @mint

    let address = mint.msg.transaction.account
    require nft     = lookup nft_item(address)
    let raw_op  = mint.msg.opcode
    out {
        source:     mint.msg.source
        address:    account(address)
        index:      nft.index
        opcode:     (raw_op + 4294967296) if raw_op < 0 else raw_op
        collection: account(nft.collection_address)
    }
}
