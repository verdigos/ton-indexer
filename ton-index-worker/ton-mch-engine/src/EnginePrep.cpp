#include "EnginePrep.h"

#include "GenMatchers.h"
#include "MsgParse.h"  // validate_registries

#include <string>

namespace mch {

td::Result<std::shared_ptr<const MchEnginePrep>> make_engine_prep() {
  // The embedding entry point runs the same registry check as the CLI. A
  // duplicate name across
  // the three parser sources must stop startup, not resolve arbitrarily.
  TRY_STATUS(validate_registries());
  auto prep = std::make_shared<MchEnginePrep>();
  prep->matchers = &gen_matchers_ir();
  prep->setup = prepare_classify(*prep->matchers);
  if (!prep->setup.error.empty()) {
    // An unrunnable generated table is a build bug. Surface it
    // as an error so the caller can be startup-fatal in production.
    return td::Status::Error(prep->setup.error);
  }
  if (!prep->setup.skips.empty()) {
    // A skipped matcher is a host name the engine does not provide: the matcher
    // would just be absent from the run, and only the goldens would notice.
    std::string error = "matchers skipped (missing host names):";
    for (const auto &[name, reason] : prep->setup.skips) {
      error += " " + name + " (" + reason + ")";
    }
    return td::Status::Error(error);
  }
  return std::shared_ptr<const MchEnginePrep>(std::move(prep));
}

}  // namespace mch
