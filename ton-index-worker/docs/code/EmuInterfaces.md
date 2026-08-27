---
module: EmuInterfaces
source: ton-index-worker/ton-mch-engine/src/emu/EmuInterfaces.cpp
source-commit: 06d4c50
---

# EmuInterfaces

Field-by-field converters from emulator detector result structs to the schema V2 data structs consumed by lookup handlers. Each is a pure function that copies the fields the V2 side carries and deliberately drops the rest — lazy cells that could outlive their backing storage, factory-verification inputs, and fields with no compatible type. Called from make_interface_map for tier-1 resolution and from the celldb tier-2 resolver. No state, no error paths, no allocation beyond the returned struct.

## to_v2

`schema::GetGemsNftFixPriceSaleV4Data to_v2(const GetGemsNftFixPriceSaleV4::Result & r)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuInterfaces.cpp:112`
- callers: [mch::make_interface_map (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierBridge.cpp:64)](EmuClassifierBridge.md#make_interface_map)
- calls: `schema::GetGemsNftFixPriceSaleV4Data::GetGemsNftFixPriceSaleV4Data`

Copies the V4 fixed-price-sale fields into the V2 data struct.

Field-for-field copy of the shared sale fields — addresses, completion flag, creation time, full price, and the marketplace-fee and royalty pairs — plus the V4-only sold_at, sold_query_id, and jetton_price_dict. Nothing is dropped.

## to_v2

`schema::MultisigOrderData to_v2(const MultisigOrder::Result & r)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuInterfaces.cpp:131`
- callers: [mch::make_interface_map (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierBridge.cpp:69)](EmuClassifierBridge.md#make_interface_map)
- calls: `schema::MultisigOrderData::MultisigOrderData`

Copies multisig order fields into the V2 data struct, dropping the order cell.

Copies addresses, order sequence number, threshold, execution flag, approvals mask and count, and expiration date, plus signers. The order field is deliberately not copied: it can be a lazy cell backed by the source block data, and classifier interfaces live across trace patches, so reading it later could touch a destroyed bag-of-cells database. Only signers is read downstream.

## to_v2

`schema::NFTItemDataV2 to_v2(const NftItemDetectorR::Result & r)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuInterfaces.cpp:15`
- callers: [mch::EmuCelldbTier2::resolve (ton-index-worker/ton-mch-engine/src/emu/EmuCelldbLookup.cpp:78)](EmuCelldbLookup.md#resolve); [mch::make_interface_map (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierBridge.cpp:47)](EmuClassifierBridge.md#make_interface_map)
- calls: `schema::NFTItemDataV2::NFTItemDataV2`

Copies NFT item detector fields into the V2 data struct.

Copies address, init, index, collection and owner addresses, and content, which must remain an optional dictionary because get_nft_data requires it. dns_entry has no compatible type and is not read by lookup handlers, so it is dropped.

## to_v2

`schema::GetGemsNftFixPriceSaleData to_v2(const GetGemsNftFixPriceSale::Result & r)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuInterfaces.cpp:28`
- callers: [mch::make_interface_map (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierBridge.cpp:52)](EmuClassifierBridge.md#make_interface_map)
- calls: `schema::GetGemsNftFixPriceSaleData::GetGemsNftFixPriceSaleData`

Copies the fixed-price-sale fields into the V2 data struct.

Field-for-field copy of address, completion flag, creation time, marketplace and NFT addresses, full price, and the marketplace-fee and royalty pairs; nothing is dropped.

## to_v2

`schema::GetGemsNftAuctionData to_v2(const GetGemsNftAuction::Result & r)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuInterfaces.cpp:44`
- callers: [mch::EmuCelldbTier2::resolve (ton-index-worker/ton-mch-engine/src/emu/EmuCelldbLookup.cpp:88)](EmuCelldbLookup.md#resolve); [mch::make_interface_map (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierBridge.cpp:57)](EmuClassifierBridge.md#make_interface_map)
- calls: `schema::GetGemsNftAuctionData::GetGemsNftAuctionData`

Copies the full auction state into the V2 data struct.

Copies addresses, ending and step timing, bids, fee factors and bases for marketplace and royalty, jetton wallet and master, and the broken-state flag and public key; nothing is dropped.

## to_v2

`schema::JettonWalletDataV2 to_v2(const JettonWalletDetectorR::Result & r)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuInterfaces.cpp:5`
- callers: [mch::EmuCelldbTier2::resolve (ton-index-worker/ton-mch-engine/src/emu/EmuCelldbLookup.cpp:98)](EmuCelldbLookup.md#resolve); [mch::make_interface_map (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierBridge.cpp:45)](EmuClassifierBridge.md#make_interface_map)
- calls: `schema::JettonWalletDataV2::JettonWalletDataV2`

Copies balance, address, owner, jetton, and the mintless-claimed flag into the V2 struct; nothing is dropped.


## to_v2

`schema::DedustPoolData to_v2(const DedustPoolDetector::Result & r)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuInterfaces.cpp:76`
- callers: [mch::EmuCelldbTier2::resolve (ton-index-worker/ton-mch-engine/src/emu/EmuCelldbLookup.cpp:108)](EmuCelldbLookup.md#resolve); [mch::make_interface_map (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierBridge.cpp:68)](EmuClassifierBridge.md#make_interface_map)
- calls: `schema::DedustPoolData::DedustPoolData`

Copies DeDust pool fields into the V2 data struct.

Copies both assets and reserves, the stable flag, and the fee. The asset slice fields stay behind: they are the factory-verification input, not part of the V2 struct, and nothing downstream of the lookup reads them.

## to_v2

`schema::NominatorPoolData to_v2(const NominatorPoolContract::Result & r)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuInterfaces.cpp:90`
- callers: [mch::EmuCelldbTier2::resolve (ton-index-worker/ton-mch-engine/src/emu/EmuCelldbLookup.cpp:115)](EmuCelldbLookup.md#resolve); [mch::make_interface_map (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierBridge.cpp:70)](EmuClassifierBridge.md#make_interface_map)
- calls: `schema::NominatorPoolData::NominatorPoolData`

Copies nominator pool state into the V2 data struct, reshaping the nominator list.

Copies pool state, nominator counts, stake bounds, and validator fields. Each nominator entry is reshaped into the V2 form with balance from the amount field and pending_balance from the pending deposit amount.
