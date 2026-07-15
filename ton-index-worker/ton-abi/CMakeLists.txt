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
)
target_include_directories(ton-abi PUBLIC src)
target_compile_features(ton-abi PUBLIC cxx_std_20)
target_link_libraries(ton-abi PUBLIC tdutils ton_crypto_core)

add_executable(ton-abi-test
    test/smoke_test.cpp
    test/VectorRunner.cpp
    test/AbiLoaderTest.cpp
)
target_include_directories(ton-abi-test PRIVATE test)
target_compile_features(ton-abi-test PRIVATE cxx_std_20)
target_compile_definitions(ton-abi-test PRIVATE
    TON_ABI_VECTORS_FILE="${CMAKE_CURRENT_SOURCE_DIR}/testdata/abi_vectors.json"
    TON_ABI_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata/fixtures"
)
target_link_libraries(ton-abi-test ton-abi tdutils)

enable_testing()
add_test(NAME ton_abi_smoke COMMAND ton-abi-test)
