// The write-back payload: everything the emulator's Redis MULTI needs, already
// serialized.
//
// Deliberately its own file with only the standard-library includes below, so
// ton-trace-emulator's TraceInserter.h can include it without pulling Value.h /
// ActionBuild.h / EnginePrep.h into the insert TU. Built on the
// classifier actor while the cell anchor is still alive; no mch value type
// survives into the insert.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mch {

struct EmuActionPayload {
  // Bare msgpack (uncompressed — td::Gzip::init_encode calls deflateInit2 with
  // windowBits=15, i.e. ZLIB framing 0x78 0x9c, not gzip 0x1f 0x8b;
  // ton-index-go sniffs the two-byte gzip magic and on a miss hands the bytes
  // straight to msgpack.Unmarshal, which would fail and drop the trace. Its
  // DECODER auto-detects both (inflateInit2 with MAX_WBITS + 32), which is
  // exactly what makes the asymmetry easy to miss. A later "optimisation" that
  // reaches for td::gzencode reintroduces the bug; real gzip framing
  // (deflateInit2 with 15 + 16) is the correct route if compression is ever
  // wanted.)
  // EMPTY == classification did not produce a payload: the `actions` field is
  // NOT written.
  std::string actions_blob;
  // (account, "<trace_key>:<action_id>") for every (action, account) pair.
  std::vector<std::pair<std::string, std::string>> aai;
  // Accounts that appear in an action but in no transaction of the trace.
  std::vector<std::string> referenced_accounts;
  std::int64_t aai_score{0};    // Python's trace.start_lt == root tx lt
  std::size_t action_count{0};  // telemetry only
  // Written to the trace hash as `mch_classify_state`. NULL means the
  // classifier never saw this emission at all (feature off) — the one case
  // where the field must be ABSENT rather than carry a placeholder. Every other
  // path, gate-side sheds included, sets it explicitly.
  const char *state{nullptr};
  // The emission's finality: `min` over the view's node finalities, and so the
  // same value serialize_actions puts on the wire as the `finality` key. The
  // insert stores it beside the blob and refuses an overwrite from anything
  // less advanced — the node loop has guarded itself that way since
  // TraceInserter.cpp:87-95; `actions` is the one field that never did.
  std::uint8_t finality{0};
};

// The guard's whole decision. A pure function rather than an `if` inside
// TraceInserter's body for one reason: it is the rule that keeps a trace and
// its actions describing the SAME emission, and the two branches that are easy
// to get backwards — absent and unparseable — both have to mean "write", not
// "skip". A guard that fails closed would wedge the actions field permanently
// on the first corrupt value.
//
// `stored` is the raw `actions_finality` hash value as Redis returns it
// (nullopt == the field is absent, i.e. no actions are stored yet).
inline bool actions_write_is_stale(const std::optional<std::string> &stored,
                                   std::uint8_t emission_finality) {
  if (!stored) {
    return false;  // nothing stored: nothing to downgrade
  }
  int stored_finality = -1;
  try {
    stored_finality = std::stoi(*stored);
  } catch (const std::exception &) {
    return false;  // unreadable: treat as absent rather than block writes forever
  }
  return stored_finality > static_cast<int>(emission_finality);
}

}  // namespace mch
