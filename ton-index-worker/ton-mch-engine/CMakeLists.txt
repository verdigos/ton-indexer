cmake_minimum_required(VERSION 3.16)

# --- mch-codegen: build-time IR -> C++ source generator (no Python in the build).
# Reads the version-controlled IR JSON under ir/ and emits deterministic C++.
add_executable(mch-codegen
    codegen/main.cpp
    codegen/ExprCodegen.cpp
)
target_compile_features(mch-codegen PRIVATE cxx_std_20)
target_link_libraries(mch-codegen tdutils)

# --- generated sources ---------------------------------------------------------
# EVERY generated file (mch-codegen tables + tlbc parsers) is a build step whose
# output lives in the build tree (${CMAKE_CURRENT_BINARY_DIR}/generated/), never
# in the source tree or git. The ir/*.json inputs and tlb/mch-msgs.tlb ARE the
# version-controlled contract; the .cpp tables are derived artifacts.
#
# Per-document build tables: mch-codegen --builds emits gen_builds_<suffix>()
# symbols, so the table generated from ir/mch_ir.json (classify / build-smoke)
# and the one from ir/build_vectors.json (--build-vectors) both link into the
# binary, each permanently paired with its input document. The runners' sha256
# guards remain as a safety net but now pass by construction.
set(MCH_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${MCH_GEN_DIR})

add_custom_command(
    OUTPUT ${MCH_GEN_DIR}/vectors_generated.cpp
    COMMAND mch-codegen --vectors ${CMAKE_CURRENT_SOURCE_DIR}/ir/expr_vectors.json
            -o ${MCH_GEN_DIR}/vectors_generated.cpp
    DEPENDS mch-codegen ${CMAKE_CURRENT_SOURCE_DIR}/ir/expr_vectors.json
    COMMENT "Generate MCH expression conformance functions (mch-codegen --vectors)"
)
add_custom_command(
    OUTPUT ${MCH_GEN_DIR}/wheres_generated.cpp
    COMMAND mch-codegen --wheres ${CMAKE_CURRENT_SOURCE_DIR}/ir/mch_ir.json
            -o ${MCH_GEN_DIR}/wheres_generated.cpp
    DEPENDS mch-codegen ${CMAKE_CURRENT_SOURCE_DIR}/ir/mch_ir.json
    COMMENT "Generate MCH where_expr functions (mch-codegen --wheres)"
)
add_custom_command(
    OUTPUT ${MCH_GEN_DIR}/builds_ir_generated.cpp
    COMMAND mch-codegen --builds ${CMAKE_CURRENT_SOURCE_DIR}/ir/mch_ir.json
            --suffix ir -o ${MCH_GEN_DIR}/builds_ir_generated.cpp
    DEPENDS mch-codegen ${CMAKE_CURRENT_SOURCE_DIR}/ir/mch_ir.json
    COMMENT "Generate MCH build-program functions from the IR artifact (mch-codegen --builds)"
)
add_custom_command(
    OUTPUT ${MCH_GEN_DIR}/builds_vectors_generated.cpp
    COMMAND mch-codegen --builds ${CMAKE_CURRENT_SOURCE_DIR}/ir/build_vectors.json
            --suffix vectors -o ${MCH_GEN_DIR}/builds_vectors_generated.cpp
    DEPENDS mch-codegen ${CMAKE_CURRENT_SOURCE_DIR}/ir/build_vectors.json
    COMMENT "Generate MCH build-program functions from the build vectors (mch-codegen --builds)"
)

# TL-B jetton-family parsers from tlb/mch-msgs.tlb via tlbc (same build-tree
# discipline as the mch-codegen outputs above).
set(MCH_MSGS_TLB
    ${MCH_GEN_DIR}/mch-msgs-tlb.cpp
    ${MCH_GEN_DIR}/mch-msgs-tlb.h
)
add_custom_command(
    WORKING_DIRECTORY ${MCH_GEN_DIR}
    COMMAND tlbc -o mch-msgs-tlb -n mchmsg::gen -z ${CMAKE_CURRENT_SOURCE_DIR}/tlb/mch-msgs.tlb
    COMMENT "Generate mch-msgs tlb source files"
    OUTPUT ${MCH_MSGS_TLB}
    DEPENDS tlbc ${CMAKE_CURRENT_SOURCE_DIR}/tlb/mch-msgs.tlb
)

# OPTIONAL testbed table (-DMCH_TESTBED_IR=<artifact.json>): lets a phase run
# classify / build-smoke against an experimental artifact WITHOUT editing the
# ir/mch_ir.json contract — runners select the table by source sha
# (select_gen_builds). Unset (default): the committed empty stub is compiled.
set(MCH_TESTBED_IR "" CACHE FILEPATH
    "Optional experimental IR artifact; generates a third build table (gen_builds_testbed)")
if(MCH_TESTBED_IR)
    add_custom_command(
        OUTPUT ${MCH_GEN_DIR}/builds_testbed_generated.cpp
        COMMAND mch-codegen --builds ${MCH_TESTBED_IR}
                --suffix testbed -o ${MCH_GEN_DIR}/builds_testbed_generated.cpp
        DEPENDS mch-codegen ${MCH_TESTBED_IR}
        COMMENT "Generate MCH build-program functions from the TESTBED artifact (mch-codegen --builds)"
    )
    set(MCH_TESTBED_SOURCE ${MCH_GEN_DIR}/builds_testbed_generated.cpp)
else()
    set(MCH_TESTBED_SOURCE src/BuildsTestbedStub.cpp)
endif()

set(MCH_GENERATED_SOURCES
    ${MCH_GEN_DIR}/vectors_generated.cpp
    ${MCH_GEN_DIR}/wheres_generated.cpp
    ${MCH_GEN_DIR}/builds_ir_generated.cpp
    ${MCH_GEN_DIR}/builds_vectors_generated.cpp
    ${MCH_TESTBED_SOURCE}
    ${MCH_GEN_DIR}/mch-msgs-tlb.cpp
)
add_custom_target(mch_generate_sources DEPENDS ${MCH_GENERATED_SOURCES} ${MCH_GEN_DIR}/mch-msgs-tlb.h)

# --- mch-classify: the classification CORE static library (Phase G step 3,
# prod-design §2). The pure pipeline TUs (no CLI/dump/harness) — ton-index-
# postgres will link THIS; the CLI exe below links it + the harness TUs. Slice
# 1 is ADDITIVE: identical objects, split into a lib for reuse. (build_action /
# fill_* Action-row rendering stays with the harness Classify.cpp for now — it
# moves lib-side in the slice that wires the production actor.)
add_library(mch-classify STATIC
    src/TraceLoader.cpp
    src/BlockTree.cpp
    src/Value.cpp
    src/ExprRuntime.cpp
    src/ExprEval.cpp
    src/IrLoader.cpp
    src/Walker.cpp
    src/HostRegistry.cpp
    src/host/HostCommon.cpp
    src/host/HostGetgems.cpp
    src/host/HostNominator.cpp
    src/host/HostEvaa.cpp
    src/host/HostDedust.cpp
    src/host/HostLayerZero.cpp
    src/host/HostTonstakers.cpp
    src/host/HostJvault.cpp
    src/host/HostNft.cpp
    src/host/HostTgbtc.cpp
    src/host/HostCoffee.cpp
    src/host/HostStonfi.cpp
    src/host/HostDedustDeposit.cpp
    src/host/HostTonco.cpp
    src/BuildRuntime.cpp
    src/BuildDriver.cpp
    src/ClassifyCore.cpp
    src/MsgParse.cpp
    src/parse/PSlice.cpp
    src/parse/ParseJettons.cpp
    src/parse/ParseDns.cpp
    src/parse/ParseVesting.cpp
    src/parse/ParsePton.cpp
    src/parse/ParseStonfi.cpp
    src/parse/ParseDedust.cpp
    src/parse/ParseCoffee.cpp
    src/parse/ParseEvaa.cpp
    src/parse/ParseLayerZero.cpp
    src/parse/ParseJvault.cpp
    ${MCH_GENERATED_SOURCES}
)
target_include_directories(mch-classify
    PUBLIC src
    # MsgParse.cpp includes "generated/mch-msgs-tlb.h" -> resolves into the
    # build tree; nothing generated ever lands under src/.
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
)
target_compile_features(mch-classify PUBLIC cxx_std_20)

find_package(lz4 CONFIG REQUIRED)
target_link_libraries(mch-classify PUBLIC tondb-scanner lz4::lz4)
add_dependencies(mch-classify mch_generate_sources)

# The CLI/dev tool: harness TUs (dump/runners/main) over the core lib.
add_executable(ton-mch-engine
    src/main.cpp
    src/VectorRunner.cpp
    src/MatchRunner.cpp
    src/BuildVectorRunner.cpp
    src/BuildSmoke.cpp
    src/Classify.cpp
    src/ShapeTest.cpp
    src/ParseDump.cpp
)
target_include_directories(ton-mch-engine PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})
target_compile_features(ton-mch-engine PRIVATE cxx_std_20)
target_link_libraries(ton-mch-engine mch-classify)
add_dependencies(ton-mch-engine mch_generate_sources)

install(TARGETS ton-mch-engine RUNTIME DESTINATION bin)

# --- CTest wiring (§10.1) ------------------------------------------------------
# The deterministic, self-contained gate modes as native tests (`ctest -R mch`).
# The differential A/B gates (parse-dump / classify vs the Python twins) stay
# OUT — they need the fixture corpus and Python and are run from the harness.
enable_testing()
add_test(NAME mch_expr_vectors
    COMMAND ton-mch-engine --vectors ${CMAKE_CURRENT_SOURCE_DIR}/ir/expr_vectors.json)
add_test(NAME mch_expr_vectors_gen
    COMMAND ton-mch-engine --vectors-gen ${CMAKE_CURRENT_SOURCE_DIR}/ir/expr_vectors.json)
add_test(NAME mch_build_vectors
    COMMAND ton-mch-engine --build-vectors ${CMAKE_CURRENT_SOURCE_DIR}/ir/build_vectors.json)
add_test(NAME mch_shape_test
    COMMAND ton-mch-engine --shape-test)
add_test(NAME mch_codegen_selftest
    COMMAND mch-codegen --selftest)
