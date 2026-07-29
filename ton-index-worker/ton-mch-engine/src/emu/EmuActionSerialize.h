// EM2 W1 (em2-writeback-design.md §2, §9): mch::Action rows -> the msgpack
// bytes ton-index-go decodes out of the pending trace hash's `actions` field
// (index/emulated/models.go:530-601, :821-836).
//
// Pure library function, no actor and no Redis: W3 calls it from
// EmuClassifierActor::classify (§3) and puts the result in EmuActionPayload;
// W2's offline twin gate calls it from the harness over the fixture corpus.
#pragma once

#include "ActionBuild.h"
#include "EmuTypes.h"

#include <cstddef>
#include <string>
#include <vector>

namespace mch {

// Value shapes §2.7 says should never reach an Action. They are serialized
// anyway (a dropped field would be a silent wire hole) and COUNTED, so a future
// fill that starts producing one is visible instead of merely surprising.
struct ActionSerializeStats {
  std::size_t float_values{0};  // VType::Float — Python str(float); no ported fill emits one
  std::size_t cell_values{0};   // VType::Cell — base64(BOC); an Action field should be Bytes
  std::size_t unrenderable{0};  // null RefInt256 / null cell / BOC failure -> msgpack nil
};

// UNCOMPRESSED msgpack array of per-action maps. An empty `actions` yields the
// one-byte empty array, NOT an empty string: "no actions" and "no payload" are
// different states on the wire (§7), and EmuActionPayload::actions_blob uses
// empty to mean the latter.
//
// `view` supplies the two wire keys that are not on the Action:
// `trace_external_hash_norm` (= view.trace_id, the Redis trace key) and
// `finality` (= min over the view's node finalities, matching Python's
// min(tx.finality)).
std::string serialize_actions(const std::vector<Action> &actions, const EmuTraceView &view,
                              ActionSerializeStats *stats = nullptr);

}  // namespace mch
