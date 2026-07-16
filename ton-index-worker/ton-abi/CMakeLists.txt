cmake_minimum_required(VERSION 3.16)

# --- ton-abi: standalone C++ port of the Tolk ABI message (de)serialization
# rules (docs/ton-abi-plan.md). Pure C++, self-contained (decision 2): no
# ton-mch-engine dependency, ever (decision 5).
# W0 (scaffold): lib + test exe + vendored doctest + ctest wiring.
# W1 (T1.1/T1.2): AbiModel.h (header-only PODs) + AbiLoader.h/.cpp (JSON ->
# AbiModel, fail-closed). AbiKernel/AbiLeaves/AbiValue/AbiInterp/AbiEmit land
# in later waves.

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
    src/AbiInterp.cpp
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
    test/VectorRunner.cpp
    test/AbiLoaderTest.cpp
    test/AbiKernelTest.cpp
    test/AbiLeavesScalarTest.cpp
    test/AbiLeavesAddressTest.cpp
    test/AbiLeavesRefTest.cpp
    test/AbiLeavesContainerTest.cpp
    test/AbiLeavesDictTest.cpp
    test/AbiLeavesPrefixTest.cpp
    test/AbiValueTest.cpp
    test/AbiInterpTest.cpp
    test/AbiInterpW9Test.cpp
    test/AbiW10Test.cpp
    test/AbiEmitTest.cpp
    # committed generated pairs (A/B gate input; W11 T11.1 subset compiled here)
    generated/tolk_counter_gen.cpp
    generated/lots_of_messages_gen.cpp
    generated/generic_union_labels_gen.cpp
)
target_include_directories(ton-abi-test PRIVATE test generated)
target_compile_features(ton-abi-test PRIVATE cxx_std_20)
target_compile_definitions(ton-abi-test PRIVATE
    TON_ABI_VECTORS_FILE="${CMAKE_CURRENT_SOURCE_DIR}/testdata/abi_vectors.json"
    TON_ABI_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata/fixtures"
)
target_link_libraries(ton-abi-test ton-abi tdutils)

enable_testing()
add_test(NAME ton_abi_smoke COMMAND ton-abi-test)
