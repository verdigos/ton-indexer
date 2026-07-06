cmake_minimum_required(VERSION 3.16)

# --- mch-codegen: build-time IR -> C++ source generator (no Python in the build).
# Reads the version-controlled IR JSON under ir/ and emits deterministic C++.
add_executable(mch-codegen
    codegen/main.cpp
    codegen/ExprCodegen.cpp
)
target_compile_features(mch-codegen PRIVATE cxx_std_20)
target_link_libraries(mch-codegen tdutils)

# Generated expression-conformance functions + table (mirrors the tlbc/tokens
# codegen precedent in tondb-scanner/CMakeLists.txt). Committed in-tree AND
# regenerable: the custom command rebuilds it when mch-codegen or the input JSON
# changes.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/generated/vectors_generated.cpp
    COMMAND mch-codegen --vectors ${CMAKE_CURRENT_SOURCE_DIR}/ir/expr_vectors.json
            -o ${CMAKE_CURRENT_SOURCE_DIR}/src/generated/vectors_generated.cpp
    DEPENDS mch-codegen ${CMAKE_CURRENT_SOURCE_DIR}/ir/expr_vectors.json
    COMMENT "Generate MCH expression conformance functions (mch-codegen)"
)
add_custom_target(mch_codegen_vectors
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/generated/vectors_generated.cpp)

add_executable(ton-mch-engine
    src/main.cpp
    src/TraceLoader.cpp
    src/BlockTree.cpp
    src/Value.cpp
    src/ExprRuntime.cpp
    src/ExprEval.cpp
    src/VectorRunner.cpp
    src/IrLoader.cpp
    src/Walker.cpp
    src/MatchRunner.cpp
    src/BuildRuntime.cpp
    src/BuildVectorRunner.cpp
    src/BuildDriver.cpp
    src/BuildSmoke.cpp
    src/Classify.cpp
    src/generated/vectors_generated.cpp
    src/generated/wheres_generated.cpp
    src/generated/builds_generated.cpp
)
target_include_directories(ton-mch-engine
    PUBLIC src
)
target_compile_features(ton-mch-engine PRIVATE cxx_std_20)

find_package(lz4 CONFIG REQUIRED)
target_link_libraries(ton-mch-engine tondb-scanner lz4::lz4)

# Ensure the generated source is (re)built before ton-mch-engine compiles it.
add_dependencies(ton-mch-engine mch_codegen_vectors)

install(TARGETS ton-mch-engine RUNTIME DESTINATION bin)

# --- TL-B message-parsing layer (MsgParse) -----------------------------------
# Generated jetton-family parsers from tlb/mch-msgs.tlb via tlbc (clones the
# tondb-scanner tokens-tlb precedent). Committed in-tree AND regenerable.
# Keep this section contiguous and at the END of the file.
set(MCH_MSGS_TLB
  ${CMAKE_CURRENT_SOURCE_DIR}/src/generated/mch-msgs-tlb.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/generated/mch-msgs-tlb.h
)
add_custom_command(
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
    COMMAND tlbc -o mch-msgs-tlb -n mchmsg::gen -z ${CMAKE_CURRENT_SOURCE_DIR}/tlb/mch-msgs.tlb
    COMMENT "Generate mch-msgs tlb source files"
    OUTPUT ${MCH_MSGS_TLB}
    DEPENDS tlbc ${CMAKE_CURRENT_SOURCE_DIR}/tlb/mch-msgs.tlb
)
add_custom_target(mch_tlb_generate_msgs DEPENDS ${MCH_MSGS_TLB})
add_dependencies(ton-mch-engine mch_tlb_generate_msgs)
target_sources(ton-mch-engine PRIVATE
    src/MsgParse.cpp
    src/generated/mch-msgs-tlb.cpp
)
# --- end TL-B message-parsing layer -------------------------------------------
