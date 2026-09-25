// Adapts emulator detector results to the pipeline's schema V2 structs for
// tier-1 lookup: one to_schema() overload per detector. Fields unavailable from
// detectors remain value-initialized.
#pragma once

#include "IndexData.h"  // schema::*V2 + the detector Result types

namespace mch {

schema::JettonWalletDataV2 to_schema(const JettonWalletDetectorR::Result &r);
schema::NFTItemDataV2 to_schema(const NftItemDetectorR::Result &r);  // content carried
schema::GetGemsNftFixPriceSaleData to_schema(const GetGemsNftFixPriceSale::Result &r);
schema::GetGemsNftAuctionData to_schema(const GetGemsNftAuction::Result &r);
// The cell-db lookup uses the same adapters as tier 1.
schema::DedustPoolData to_schema(const DedustPoolDetector::Result &r);
schema::NominatorPoolData to_schema(const NominatorPoolContract::Result &r);
schema::GetGemsNftFixPriceSaleV4Data to_schema(const GetGemsNftFixPriceSaleV4::Result &r);
schema::MultisigOrderData to_schema(const MultisigOrder::Result &r);
schema::TelemintData to_schema(const TelemintContract::Result &r);

// Detector results omit code hashes, but GetGems version lookup requires one.
// Set it on NFT variants only. Missing account code leaves zero, which renders
// as a lookup miss rather than a wrong version.
void set_nft_code_hash(schema::BlockchainInterfaceV2 &iface, const schema::AccountState &state);
void set_nft_code_hash(schema::BlockchainInterfaceV2 &iface, const td::Bits256 &code_hash);

}  // namespace mch
