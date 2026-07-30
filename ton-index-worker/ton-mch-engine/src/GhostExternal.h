// GHOST-EXTERNAL fallback (event_processing.py init_from_external :159-196 +
// matchers_for_failed_externals :314-316, blocks/messages/externals.py).
//
// A trace that is ONE ext-in transaction with no children serializes nothing —
// its only block is a call_contract with a null source, which serialize_blocks
// drops — so Python would ship the synthetic `unknown` row. For a WALLET
// external that is a lie by omission: the body still carries the messages the
// wallet meant to send and never did (the tx aborted, or it was rejected before
// the action phase). Python re-reads that body, hangs one GHOST child node per
// unsent message off the external's node, and serializes those instead.
//
// The two halves live here because neither is expressible as an MCH spec: the
// first invents tree nodes (specs match an existing tree), and the second is a
// matcher Python runs ONLY on this fallback path — as a registered spec matcher
// it would also fire on the main spine, turning every lone unmatched
// jetton-transfer call into a jetton_transfer row.
#pragma once

#include "BlockTree.h"

#include <cstddef>

namespace mch {

class LookupSource;  // BuildRuntime.h

// init_from_external (event_processing.py:159-196). Forces `root` failed, parses
// its message body as a wallet v3/v4/v5r1 external and appends one ghost child
// per decoded payload. Returns the number of children appended — 0 when the body
// is not a wallet external, carries no messages, or does not parse (Python:
// extract_payload_from_wallet_message returns []). Never throws.
std::size_t synthesize_ghost_children(EventTree &tree, EventNode *root);

// FallbackJettonTransferBlockMatcher (blocks/jettons.py:282-330), the sole entry
// of matchers_for_failed_externals: a jetton-transfer call becomes a
// jetton_transfer block whose asset comes from the SENDER's wallet interface
// (there is no internal-transfer leg to read the receiver's from). Returns the
// produced block, or nullptr when `block` is not a jetton-transfer call or the
// build would have raised (Python: try_build catches and returns None).
Block *fallback_jetton_transfer(Block *block, BlockArena &arena, const LookupSource &src);

}  // namespace mch
