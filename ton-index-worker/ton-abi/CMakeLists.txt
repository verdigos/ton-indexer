cmake_minimum_required(VERSION 3.16)

# --- ton-abi: standalone C++ port of the Tolk ABI message (de)serialization
# rules (docs/ton-abi-plan.md). W0 = scaffold only: lib + test exe + vendored
# doctest + ctest wiring. Real content (AbiModel/AbiLoader/AbiKernel/...)
# lands wave by wave starting W1.

add_library(ton-abi STATIC
    src/AbiModel.cpp
)
target_include_directories(ton-abi PUBLIC src)
target_compile_features(ton-abi PUBLIC cxx_std_20)

add_executable(ton-abi-test
    test/smoke_test.cpp
    test/VectorRunner.cpp
)
target_include_directories(ton-abi-test PRIVATE test)
target_compile_features(ton-abi-test PRIVATE cxx_std_20)
target_compile_definitions(ton-abi-test PRIVATE
    TON_ABI_VECTORS_FILE="${CMAKE_CURRENT_SOURCE_DIR}/testdata/abi_vectors.json"
)
target_link_libraries(ton-abi-test ton-abi tdutils)

enable_testing()
add_test(NAME ton_abi_smoke COMMAND ton-abi-test)
