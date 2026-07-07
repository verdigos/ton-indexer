// Tonstakers host fn (builders/tonstakers.py). See host/HostImpls.h for the
// internal registry surface and HostRegistry.h for the public one.
#include "host/HostImpls.h"

#include "host/HostCommon.h"

#include "BlockTree.h"
#include "TraceLoader.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace mch {

// builders/tonstakers.py _tonstakers_minted_nft(delayed): the minted request
// NFT's address off delayed.next_blocks. The isinstance(NftMintBlock) probe is
// DEAD under the IR engine (legacy class; nft matchers are builder-tier and
// never run here), so only the TONStakersInitNFT call_contract fallback is
// live. None/absent -> null (the delayed arm then marks the block failed).
EvalResult tonstakers_minted_nft(BuildEnv &, const std::vector<Value> &args) {
  if (args.size() != 1) {
    return rt_fault("tonstakers_minted_nft: bad arguments");
  }
  const Block *delayed = as_block(args[0]);
  if (delayed == nullptr) {
    return rt_ok(Value::null());
  }
  constexpr std::uint32_t kTonstakersInitNft = 0x132F9A45;
  for (const Block *n : delayed->next_blocks) {
    if (is_call_op(n, kTonstakersInitNft)) {
      const Message *m = block_msg(n);
      return rt_ok(account_from_opt(m != nullptr ? m->destination : std::nullopt));
    }
  }
  return rt_ok(Value::null());
}

}  // namespace mch
