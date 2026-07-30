#include "EnginePrep.h"

#include "GenMatchers.h"
#include "MsgParse.h"  // validate_registries

namespace mch {

td::Result<std::shared_ptr<const MchEnginePrep>> make_engine_prep() {
  // W12 (bridge design §4): the embedding entry point runs the same advisory
  // registry check the CLI does — with live ABI rows a duplicate name across
  // the three parser sources must stop startup, not resolve arbitrarily.
  TRY_STATUS(validate_registries());
  auto prep = std::make_shared<MchEnginePrep>();
  prep->matchers = &gen_matchers_ir();
  prep->setup = prepare_classify(*prep->matchers);
  if (prep->setup.table_missing || prep->setup.fn_missing) {
    // prod-design §4: an unrunnable generated table is a build bug — surface it
    // as an error so the caller can be startup-fatal in production.
    return td::Status::Error(prep->setup.error);
  }
  return std::shared_ptr<const MchEnginePrep>(std::move(prep));
}

}  // namespace mch
