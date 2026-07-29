// Interface adapter (em1-design §4): the emulator's detector Results are field
// PREFIXES of the pipeline's schema::*V2 structs, so ParsedBlockLookupSource
// can serve tier-1 lookups from emulated accounts unchanged. The tail
// (last_transaction_lt/now, code_hash, data_hash) is never read by tier1 and
// stays value-initialized.
#pragma once

#include "IndexData.h"  // schema::*V2 + the detector Result types

namespace mch {

schema::JettonWalletDataV2 to_v2(const JettonWalletDetectorR::Result &r);
schema::NFTItemDataV2 to_v2(const NftItemDetectorR::Result &r);  // content carried
schema::GetGemsNftFixPriceSaleData to_v2(const GetGemsNftFixPriceSale::Result &r);
schema::GetGemsNftAuctionData to_v2(const GetGemsNftAuction::Result &r);

}  // namespace mch
