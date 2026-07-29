#include "EmuInterfaces.h"

namespace mch {

schema::JettonWalletDataV2 to_v2(const JettonWalletDetectorR::Result &r) {
  schema::JettonWalletDataV2 v{};
  v.balance = r.balance;
  v.address = r.address;
  v.owner = r.owner;
  v.jetton = r.jetton;
  v.mintless_is_claimed = r.mintless_is_claimed;
  return v;
}

schema::NFTItemDataV2 to_v2(const NftItemDetectorR::Result &r) {
  schema::NFTItemDataV2 v{};
  v.address = r.address;
  v.init = r.init;
  v.index = r.index;
  v.collection_address = r.collection_address;
  v.owner_address = r.owner_address;
  // content MUST be carried: get_nft_data faults on a non-Dict, so dropping it
  // here dropped the nft_transfer row of every EMULATED trace whose NFT the
  // emulator itself detected (em2-tier2-design §6.1a — a live action loss, not a
  // replay artefact). Both sides are the same optional<map<string,string>>.
  v.content = r.content;
  // dns_entry stays unset: a distinct type on either side, and no lookup kind
  // reads it.
  return v;
}

schema::GetGemsNftFixPriceSaleData to_v2(const GetGemsNftFixPriceSale::Result &r) {
  schema::GetGemsNftFixPriceSaleData v{};
  v.address = r.address;
  v.is_complete = r.is_complete;
  v.created_at = r.created_at;
  v.marketplace_address = r.marketplace_address;
  v.nft_address = r.nft_address;
  v.nft_owner_address = r.nft_owner_address;
  v.full_price = r.full_price;
  v.marketplace_fee_address = r.marketplace_fee_address;
  v.marketplace_fee = r.marketplace_fee;
  v.royalty_address = r.royalty_address;
  v.royalty_amount = r.royalty_amount;
  return v;
}

schema::GetGemsNftAuctionData to_v2(const GetGemsNftAuction::Result &r) {
  schema::GetGemsNftAuctionData v{};
  v.address = r.address;
  v.end = r.end;
  v.end_time = r.end_time;
  v.mp_addr = r.mp_addr;
  v.nft_addr = r.nft_addr;
  v.nft_owner = r.nft_owner;
  v.last_bid = r.last_bid;
  v.last_member = r.last_member;
  v.min_step = r.min_step;
  v.mp_fee_addr = r.mp_fee_addr;
  v.mp_fee_factor = r.mp_fee_factor;
  v.mp_fee_base = r.mp_fee_base;
  v.royalty_fee_addr = r.royalty_fee_addr;
  v.royalty_fee_factor = r.royalty_fee_factor;
  v.royalty_fee_base = r.royalty_fee_base;
  v.max_bid = r.max_bid;
  v.min_bid = r.min_bid;
  v.created_at = r.created_at;
  v.last_bid_at = r.last_bid_at;
  v.is_canceled = r.is_canceled;
  v.activated = r.activated;
  v.step_time = r.step_time;
  v.last_query_id = r.last_query_id;
  v.jetton_wallet = r.jetton_wallet;
  v.jetton_master = r.jetton_master;
  v.is_broken_state = r.is_broken_state;
  v.public_key = r.public_key;
  return v;
}

}  // namespace mch
