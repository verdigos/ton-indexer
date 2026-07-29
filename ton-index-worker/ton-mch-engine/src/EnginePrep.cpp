#include "EnginePrep.h"

#include "MsgParse.h"  // validate_registries

namespace mch {

td::Result<std::shared_ptr<const MchEnginePrep>> load_engine_prep(
    const std::string &artifact_path) {
  // W12 (bridge design §4): the embedding entry point runs the same advisory
  // registry check the CLI does — with live ABI rows a duplicate name across
  // the three parser sources must stop startup, not resolve arbitrarily.
  TRY_STATUS(validate_registries());
  TRY_RESULT(ir, load_ir(artifact_path));
  auto prep = std::make_shared<MchEnginePrep>();
  prep->ir = std::move(ir);
  prep->setup = prepare_classify(prep->ir);
  if (prep->setup.table_missing || prep->setup.fn_missing) {
    // prod-design §4: an unrunnable artifact matcher is a build bug — surface it
    // as an error so the caller can be startup-fatal in production.
    return td::Status::Error(prep->setup.error);
  }
  return std::shared_ptr<const MchEnginePrep>(std::move(prep));
}

}  // namespace mch
