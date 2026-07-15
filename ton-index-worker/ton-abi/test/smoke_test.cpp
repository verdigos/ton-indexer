#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

// See VectorRunner.cpp for why: td/utils/check.h (pulled in transitively via
// AbiLoader.h -> Status.h -> logging.h) redefines CHECK(...) as an
// abort-style macro, shadowing doctest's soft-check CHECK for the rest of
// this TU unless guarded.
#pragma push_macro("CHECK")
#undef CHECK
#include "AbiLoader.h"
#include "AbiModel.h"
#pragma pop_macro("CHECK")

TEST_CASE("smoke: ton-abi scaffold links and runs") {
  ton_abi::ContractABI abi;
  abi.contract_name = "Smoke";
  CHECK(abi.contract_name == "Smoke");

  auto r = ton_abi::load_abi_from_json("not json");
  CHECK(r.is_error());
}
