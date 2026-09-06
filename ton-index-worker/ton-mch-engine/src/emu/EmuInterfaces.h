// Adapts emulator detector results to the pipeline's schema V2 structs for
// tier-1 lookup. Fields unavailable from detectors remain value-initialized.
#pragma once

#include "IndexData.h"  // schema::*V2 + the detector Result types

namespace mch {

schema::JettonWalletDataV2 to_v2(const JettonWalletDetectorR::Result &r);
schema::NFTItemDataV2 to_v2(const NftItemDetectorR::Result &r);  // content carried
schema::GetGemsNftFixPriceSaleData to_v2(const GetGemsNftFixPriceSale::Result &r);
schema::GetGemsNftAuctionData to_v2(const GetGemsNftAuction::Result &r);
// The cell-db lookup uses the same adapters as tier 1.
schema::DedustPoolData to_v2(const DedustPoolDetector::Result &r);
schema::NominatorPoolData to_v2(const NominatorPoolContract::Result &r);
schema::GetGemsNftFixPriceSaleV4Data to_v2(const GetGemsNftFixPriceSaleV4::Result &r);
schema::MultisigOrderData to_v2(const MultisigOrder::Result &r);

// No detector Result carries a code hash, but the NFT shapes need one: the
// GetGems sale/auction version tables key on it and the DNS-item test reads it.
// Fills the four NFT variants from the account state the detector ran on and
// leaves every other variant untouched; an account without code stays zero,
// which renders as null (a miss) rather than a wrong version string.
void set_nft_code_hash(schema::BlockchainInterfaceV2 &iface, const schema::AccountState &state);

}  // namespace mch
