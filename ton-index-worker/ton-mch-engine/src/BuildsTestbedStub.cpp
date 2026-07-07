// Empty gen_builds_testbed() stub, compiled when the MCH_TESTBED_IR CMake
// cache var is UNSET (the normal state). With -DMCH_TESTBED_IR=<artifact.json>
// the build compiles a real generated table (mch-codegen --builds --suffix
// testbed) instead of this file — see GenBuilds.h and CMakeLists.txt.
#include "GenBuilds.h"

namespace mch {

const std::vector<GenBuild> &gen_builds_testbed() {
  static const std::vector<GenBuild> table = {};
  return table;
}

// Never equal to a real artifact's sha256 hex — select_gen_builds can probe
// this table unconditionally.
const char *gen_builds_testbed_source_sha() { return ""; }

}  // namespace mch
