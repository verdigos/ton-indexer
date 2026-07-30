// TODO: remove (harness): the IR artifact PARSER. Production no longer loads a
// document at run time — mch-codegen compiles the artifact into the matcher
// tables (GenMatchers.h), which is what ClassifyCore runs. This loader survives
// as the EQUIVALENCE REFERENCE the `mch_ir_equivalence` ctest compares the
// generated table against, and as the front end mch-codegen itself parses with.
// It lives in fixtures/ and in its own target (mch-ir-load) so mch-classify — and
// therefore ton-index-postgres and ton-trace-emulator — cannot link it. Do not
// add runtime callers.
//
// MCH IR artifact loader (docs/mch-ir-spec.md §3, §5; docs/mch-engine-v2.md
// §3.1). Parses the `1.1` artifact JSON into the immutable CompiledMatcher /
// CompiledNode tables of IrTables.h, mirroring the Python reference
// (mch_ir/loader.py + engine.py build_matchers / IrMatcher.__init__). Everything
// is copied out of the JSON tree, so the decoded document need not outlive the
// load.
//
// Deliberately host-registry-free (it links against tdutils and nothing else, so
// the build-time emitter can use it): whether a matcher's match phase can run —
// i.e. whether the host supplies its `pred` / named-`where` predicates — is
// decided by match_skip_reason() (BuildDriver.h) against the LINKED registry,
// not here. This loader only harvests the referenced names into `ref_preds`.
#pragma once

#include "IrTables.h"

#include "td/utils/Status.h"

#include <string>
#include <vector>

namespace mch {

struct LoadedIr {
  std::vector<CompiledMatcher> matchers;  // priority order (stable by priority)

  // SHA-256 (hex) of the artifact file bytes — the identity the generated
  // table records (gen_matchers_ir_source_sha), so the equivalence gate can
  // prove it compared the document the build compiled in.
  std::string source_sha256;
};

// Load + compile an artifact from a JSON file path.
td::Result<LoadedIr> load_ir(const std::string &path);

}  // namespace mch
