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
// W3: the three detectors the emulator's set gains, so their results reach
// tier 1 the same way. Also the tier-2 hook's step 5 — the celldb lookup
// produces a detector Result and files it through here.
schema::DedustPoolData to_v2(const DedustPoolDetector::Result &r);
schema::NominatorPoolData to_v2(const NominatorPoolContract::Result &r);
schema::GetGemsNftFixPriceSaleV4Data to_v2(const GetGemsNftFixPriceSaleV4::Result &r);
schema::MultisigOrderData to_v2(const MultisigOrder::Result &r);

}  // namespace mch
