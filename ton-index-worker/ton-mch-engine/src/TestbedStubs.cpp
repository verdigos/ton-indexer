// Empty gen_builds_testbed() / gen_wheres_testbed() stubs, compiled when the
// MCH_TESTBED_IR CMake cache var is UNSET (the normal state). With
// -DMCH_TESTBED_IR=<artifact.json> the build compiles the real generated tables
// (mch-codegen --builds / --wheres, both --suffix testbed) instead of this file —
// see GenBuilds.h, GenWheres.h and CMakeLists.txt.
#include "GenBuilds.h"
#include "GenWheres.h"

namespace mch {

const std::vector<GenBuild> &gen_builds_testbed() {
  static const std::vector<GenBuild> table = {};
  return table;
}

const std::vector<GenWhere> &gen_wheres_testbed() {
  static const std::vector<GenWhere> table = {};
  return table;
}

// Never equal to a real artifact's sha256 hex — select_gen_builds /
// select_gen_wheres can probe these tables unconditionally.
const char *gen_builds_testbed_source_sha() { return ""; }
const char *gen_wheres_testbed_source_sha() { return ""; }

}  // namespace mch
