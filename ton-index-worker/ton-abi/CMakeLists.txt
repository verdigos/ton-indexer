cmake_minimum_required(VERSION 3.16)

# --- ton-abi: standalone C++ port of the Tolk ABI message (de)serialization
# rules (docs/ton-abi-plan.md). Pure C++, self-contained (decision 2): no
# ton-mch-engine dependency, ever (decision 5).
# W0 (scaffold): lib + test exe + vendored doctest + ctest wiring.
# W1 (T1.1/T1.2): AbiModel.h (header-only PODs) + AbiLoader.h/.cpp (JSON ->
# AbiModel, fail-closed). AbiKernel/AbiLeaves/AbiValue/AbiEmit land in later
# waves. (W11.5: the AbiInterp interpreter -- dev scaffolding -- was removed;
# end state is pure codegen, AbiValue kept as the to_abi_value dump medium.)

add_library(ton-abi STATIC
    src/AbiLoader.cpp
    src/AbiKernel.cpp
    src/AbiLeavesScalar.cpp
    src/AbiLeavesAddress.cpp
    src/AbiLeavesRef.cpp
    src/AbiLeavesContainer.cpp
    src/AbiLeavesDict.cpp
    src/AbiLeavesPrefix.cpp
    src/AbiValue.cpp
    src/AbiEmit.cpp
)
target_include_directories(ton-abi PUBLIC src)
target_compile_features(ton-abi PUBLIC cxx_std_20)
target_link_libraries(ton-abi PUBLIC tdutils ton_crypto_core)

# ton-abi-gen: the emitter CLI (W11, T11.1). Reuses AbiModel/AbiLoader/AbiKernel
# + AbiEmit; emits committed generated/<contract>_gen.{h,cpp} pairs.
add_executable(ton-abi-gen tools/AbiGenMain.cpp)
target_compile_features(ton-abi-gen PRIVATE cxx_std_20)
target_link_libraries(ton-abi-gen ton-abi tdutils)

add_executable(ton-abi-test
    test/smoke_test.cpp
    test/AbiLoaderTest.cpp
    test/AbiKernelTest.cpp
    test/AbiLeavesScalarTest.cpp
    test/AbiLeavesAddressTest.cpp
    test/AbiLeavesRefTest.cpp
    test/AbiLeavesContainerTest.cpp
    test/AbiLeavesDictTest.cpp
    test/AbiLeavesPrefixTest.cpp
    test/AbiValueTest.cpp
    test/AbiW10Test.cpp
    test/AbiEmitTest.cpp
    test/AbiABGateTest.cpp
)
target_include_directories(ton-abi-test PRIVATE test generated)

# Committed generated pairs. ALL loadable fixtures' pairs are compiled into the
# test exe (W11 T11.2): guarantees every emitted file compiles + is the A/B gate
# input. --check (add_test below) covers byte-repro for all of them.
file(GLOB TON_ABI_GENERATED CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/generated/*_gen.cpp)
target_sources(ton-abi-test PRIVATE ${TON_ABI_GENERATED})
target_compile_features(ton-abi-test PRIVATE cxx_std_20)
target_compile_definitions(ton-abi-test PRIVATE
    TON_ABI_VECTORS_FILE="${CMAKE_CURRENT_SOURCE_DIR}/testdata/abi_vectors.json"
    TON_ABI_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata/fixtures"
    TON_ABI_GEN_EXE="$<TARGET_FILE:ton-abi-gen>"
    TON_ABI_TEST_TMP="${CMAKE_CURRENT_BINARY_DIR}/gen_cli_tmp"
)
add_dependencies(ton-abi-test ton-abi-gen)  # Cli-analog test runs the exe
target_link_libraries(ton-abi-test ton-abi tdutils)

enable_testing()
add_test(NAME ton_abi_smoke COMMAND ton-abi-test)

# --check byte-repro gate (W11 T11.2): every committed generated/ pair must
# regenerate byte-identically from its ABI (msgs_gen discipline). Excludes the
# two err-invalid-map-key fixtures (intentional hard-fail, no committed pair).
file(GLOB TON_ABI_CHECK_ABIS ${CMAKE_CURRENT_SOURCE_DIR}/testdata/fixtures/*.abi.json)
list(FILTER TON_ABI_CHECK_ABIS EXCLUDE REGEX "err-invalid-map-key")
add_test(NAME ton_abi_gen_check
         COMMAND ton-abi-gen ${TON_ABI_CHECK_ABIS} --out-dir ${CMAKE_CURRENT_SOURCE_DIR}/generated --check)
