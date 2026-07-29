// Collect-pass KEY-SET gate (em2-tier2-design §10 W2). HARNESS-ONLY.
//
// `--lookup-keys` runs every fixture through the trace-level fixpoint driver and
// dumps, per trace, the set of lookup keys the engine actually DEMANDS, next to
// the set the fixture interfaces (Python's eagerly-gathered superset) can ANSWER.
//
// MISSING (fixture answers, never demanded) and SURPLUS (demanded, fixture has
// nothing) are REVIEWED OUTPUT, never assertions. The fixture map is what
// Python's eager `address_selectors` PREDICTED it might need — every
// participant's interfaces — not what its matchers read, so it overstates the
// reference three ways: trace participants no matcher queries (a 4-pool DeDust
// route looks up only the seq destination); probe artifacts (the fixture's
// jvault resolver parses ANY account's data_boc, so every NFT item "answers"
// jvault_assets); and, in the production configuration, tier-1-servable keys.
// The cross-language containment assertion lives in `twins/py_consumed.py`,
// which instruments the interface repository and compares this dump against
// what Python ACTUALLY consumed.
//
// What this gate asserts (exit 1) are the collect pass's own invariants:
// REQ-ODD == 0 — every demanded key is a single Str address, so the kvrocks key
// derivation of §3.2 holds for all of it — plus CASE == 0, no ERROR/TRACE_FAIL,
// and rounds_exhausted == 0.
//
// Tier 1 is deliberately EMPTY here, so every lookup the engine issues reaches
// the recording hook and the dump is the trace's FULL demand set — the strongest
// form of the containment claim, since anything tier 1 would have served in-pass
// is included rather than hidden. The values are still served, one round later,
// from the same fixture source, so classification converges to the same result
// the byte-identity gates pin down (W1 verified that separately: identical dumps
// under a forced multi-round run).
//
// One consequence to keep in mind when reading the dump: the set is the union
// over ROUNDS, so it also holds keys demanded only on the round-0 path where
// everything is still null. A getgems purchase asks nft_auction in round 0
// because nft_sale has not resolved yet, and never asks again once it has. That
// over-approximates — an extra fetch, never a missing one.
//
// Side product, printed per fixture: the ROUNDS line — the fixpoint's pass count
// under exactly that worst case (nothing served in-pass), which is the number
// §4.4's cap has to cover.
#pragma once

#include <string>
#include <vector>

namespace mch {

// Exit 0 when every invariant above holds, 1 otherwise (2 on a setup error).
// Fixture load failures print ERROR and are skipped, as in run_dump — but they
// count toward the exit status here, since a gate that silently skipped the
// corpus would pass.
int run_lookup_keys(const std::string &artifact_path, const std::vector<std::string> &paths);

}  // namespace mch
