#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "AbiModel.h"

TEST_CASE("smoke: ton-abi scaffold links and runs") {
  CHECK(ton_abi::scaffold_marker() == 1);
}
