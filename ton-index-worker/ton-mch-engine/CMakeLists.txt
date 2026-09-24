cmake_minimum_required(VERSION 3.16)

# --- mch-ir-load: the IR artifact parser used by mch-codegen, which compiles
# the artifact into matcher tables.
# It links tdutils and nothing else — that is what lets the build-time tool use
# it, and it is why the runnable/skip decision (which needs the host registry)
# lives in BuildDriver.cpp instead of in the loader. mch-classify does NOT link
# it: no production binary parses an IR document any more.
add_library(mch-ir-load STATIC src/fixtures/IrLoader.cpp)
target_include_directories(mch-ir-load PUBLIC src)
target_compile_features(mch-ir-load PUBLIC cxx_std_20)
target_link_libraries(mch-ir-load PUBLIC tdutils)

# --- mch-codegen: build-time IR -> C++ source generator.
# Reads generated or testbed IR JSON and emits deterministic C++.
add_executable(mch-codegen
    codegen/main.cpp
    codegen/ExprCodegen.cpp
    codegen/MatcherCodegen.cpp
)
target_compile_features(mch-codegen PRIVATE cxx_std_20)
target_link_libraries(mch-codegen tdutils mch-ir-load)

# --- generated sources ---------------------------------------------------------
# EVERY generated mch-codegen table is a build step whose output lives in the
# build tree (${CMAKE_CURRENT_BINARY_DIR}/generated/), never in the source tree
# or git. The production IR, expression vectors and .cpp tables are derived
# artifacts.
set(MCH_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(MCH_GENERATED_IR "${MCH_GEN_DIR}/mch_ir.json")
set(MCH_GENERATED_IR_SECOND "${MCH_GEN_DIR}/mch_ir.second.json")
set(MCH_EXPR_VECTORS "${MCH_GEN_DIR}/expr_vectors.json")
set(MCH_INDEXER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../indexer" CACHE PATH
    "Path to the repository indexer/ directory used for MCH IR generation")
file(MAKE_DIRECTORY "${MCH_GEN_DIR}")
set(MCH_PAYLOAD_KEYS_HEADER "${MCH_GEN_DIR}/payload_keys_generated.h")

find_package(Python3 3.10 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR
        "MCH IR generation requires Python 3.10 or newer; install python3 and make it discoverable by CMake (Python3_EXECUTABLE).")
endif()
if(NOT EXISTS "${MCH_INDEXER_ROOT}/indexer/events/mch/ir_emit.py")
    message(FATAL_ERROR
        "MCH IR generation requires the in-tree indexer Python package at ${MCH_INDEXER_ROOT}/indexer/events/mch; set MCH_INDEXER_ROOT to the repository's indexer/ directory.")
endif()

file(GLOB MCH_SPEC_SOURCES CONFIGURE_DEPENDS
    "${MCH_INDEXER_ROOT}/indexer/events/mch/specs/*.mch"
    "${MCH_INDEXER_ROOT}/indexer/events/mch/specs/lib/*.mch"
)
set(MCH_FRONTEND_PY_SOURCES
    "${MCH_INDEXER_ROOT}/indexer/__init__.py"
    "${MCH_INDEXER_ROOT}/indexer/events/__init__.py"
    "${MCH_INDEXER_ROOT}/indexer/events/mch/__init__.py"
    "${MCH_INDEXER_ROOT}/indexer/events/mch/ast_.py"
    "${MCH_INDEXER_ROOT}/indexer/events/mch/btypes.py"
    "${MCH_INDEXER_ROOT}/indexer/events/mch/builtin_signatures.py"
    "${MCH_INDEXER_ROOT}/indexer/events/mch/compile.py"
    "${MCH_INDEXER_ROOT}/indexer/events/mch/compiler.py"
    "${MCH_INDEXER_ROOT}/indexer/events/mch/diagnostics.py"
    "${MCH_INDEXER_ROOT}/indexer/events/mch/expr_ir.py"
    "${MCH_INDEXER_ROOT}/indexer/events/mch/ir_emit.py"
    "${MCH_INDEXER_ROOT}/indexer/events/mch/lexer.py"
    "${MCH_INDEXER_ROOT}/indexer/events/mch/parser.py"
    "${MCH_INDEXER_ROOT}/indexer/events/mch/registry.py"
    "${MCH_INDEXER_ROOT}/indexer/events/mch/resolver.py"
    "${MCH_INDEXER_ROOT}/indexer/events/mch/tokens.py"
)

add_custom_command(
    OUTPUT "${MCH_GENERATED_IR}" "${MCH_GENERATED_IR_SECOND}"
    COMMAND "${Python3_EXECUTABLE}" -m indexer.events.mch.ir_emit
            -o "${MCH_GENERATED_IR}"
    COMMAND "${Python3_EXECUTABLE}" -m indexer.events.mch.ir_emit
            -o "${MCH_GENERATED_IR_SECOND}"
    WORKING_DIRECTORY "${MCH_INDEXER_ROOT}"
    DEPENDS ${MCH_SPEC_SOURCES} ${MCH_FRONTEND_PY_SOURCES}
            "${MCH_INDEXER_ROOT}/indexer/events/mch/specs/manifest.yaml"
    COMMENT "Generate deterministic MCH IR from .mch specs"
    VERBATIM
)

add_custom_command(
    OUTPUT "${MCH_EXPR_VECTORS}"
    COMMAND "${CMAKE_COMMAND}" -E env "PYTHONPATH=${MCH_INDEXER_ROOT}"
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/ir/generate_expr_vectors.py"
            "${MCH_EXPR_VECTORS}"
    WORKING_DIRECTORY "${MCH_INDEXER_ROOT}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ir/generate_expr_vectors.py"
            ${MCH_FRONTEND_PY_SOURCES}
    COMMENT "Generate MCH expression conformance vectors"
    VERBATIM
)

add_custom_command(
    OUTPUT "${MCH_PAYLOAD_KEYS_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" -m indexer.events.mch.payload_keys
            --registry "${CMAKE_CURRENT_SOURCE_DIR}/payload-keys.yaml" cpp
            --out "${MCH_PAYLOAD_KEYS_HEADER}"
    WORKING_DIRECTORY "${MCH_INDEXER_ROOT}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/payload-keys.yaml"
            "${MCH_INDEXER_ROOT}/indexer/events/mch/payload_keys.py"
    COMMENT "Generate MCH compact payload key table"
    VERBATIM
)

# The document the CLASSIFY tables (matchers + builds + wheres) are generated
# from. -DMCH_TESTBED_IR=<artifact.json> lets a build run classify against an
# experimental artifact without replacing the generated production IR; the table
# SYMBOLS are unchanged, so swapping the document is all the knob does and no
# run-time table selection is needed. All three tables come from this one
# variable, which is why they cannot disagree unless a build is left half-done
# (prepare_classify cross-checks their source shas for exactly that case).
set(MCH_TESTBED_IR "" CACHE FILEPATH
    "Optional experimental IR artifact used instead of the generated production IR")
if(MCH_TESTBED_IR)
    set(MCH_CLASSIFY_ARTIFACT ${MCH_TESTBED_IR})
    message(STATUS "mch: classify tables from TESTBED artifact ${MCH_TESTBED_IR}")
else()
    set(MCH_CLASSIFY_ARTIFACT ${MCH_GENERATED_IR})
endif()

add_custom_command(
    OUTPUT ${MCH_GEN_DIR}/wheres_ir_generated.cpp
    COMMAND mch-codegen --wheres ${MCH_CLASSIFY_ARTIFACT}
            --suffix ir -o ${MCH_GEN_DIR}/wheres_ir_generated.cpp
    DEPENDS mch-codegen ${MCH_CLASSIFY_ARTIFACT}
    COMMENT "Generate MCH where_expr functions from the IR artifact (mch-codegen --wheres)"
)
add_custom_command(
    OUTPUT ${MCH_GEN_DIR}/builds_ir_generated.cpp
    COMMAND mch-codegen --builds ${MCH_CLASSIFY_ARTIFACT}
            --suffix ir -o ${MCH_GEN_DIR}/builds_ir_generated.cpp
    DEPENDS mch-codegen ${MCH_CLASSIFY_ARTIFACT}
    COMMENT "Generate MCH build-program functions from the IR artifact (mch-codegen --builds)"
)
add_custom_command(
    OUTPUT ${MCH_GEN_DIR}/btypes_gen.h
    COMMAND mch-codegen --btypes ${MCH_CLASSIFY_ARTIFACT} -o ${MCH_GEN_DIR}/btypes_gen.h
    DEPENDS mch-codegen ${MCH_CLASSIFY_ARTIFACT}
    COMMENT "Generate btype constants header (mch-codegen --btypes)"
)
add_custom_command(
    OUTPUT ${MCH_GEN_DIR}/matchers_ir_generated.cpp
    COMMAND mch-codegen --matchers ${MCH_CLASSIFY_ARTIFACT}
            --suffix ir -o ${MCH_GEN_DIR}/matchers_ir_generated.cpp
    DEPENDS mch-codegen ${MCH_CLASSIFY_ARTIFACT}
    COMMENT "Generate the MCH matcher table from the IR artifact (mch-codegen --matchers)"
)
# The generated matcher, build and where tables are linked by the classify loop.
set(MCH_GENERATED_SOURCES
    ${MCH_PAYLOAD_KEYS_HEADER}
    ${MCH_GEN_DIR}/btypes_gen.h
    ${MCH_GEN_DIR}/matchers_ir_generated.cpp
    ${MCH_GEN_DIR}/builds_ir_generated.cpp
    ${MCH_GEN_DIR}/wheres_ir_generated.cpp
)
add_custom_target(mch_generate_sources
    DEPENDS ${MCH_GENERATED_IR} ${MCH_GENERATED_IR_SECOND}
            ${MCH_GENERATED_SOURCES})
add_custom_target(mch_generate_test_vectors DEPENDS ${MCH_EXPR_VECTORS})

# --- mch-classify: the classification CORE static library (Phase G step 3,
# The pure pipeline TUs (no CLI/dump/harness) - ton-index-
# postgres links THIS; the CLI exe below links it + the harness TUs. Action-row
# construction (Action struct + build_action + the fill_*) lives here in
# ActionBuild.cpp; only the dump-side render_action + stdout printing stayed
# with the harness ClassifyDump.cpp.
# ton-abi generated pairs live here (fixture pairs for the harness test below;
# the PRODUCTION pairs compiled into mch-classify).
set(TON_ABI_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/../ton-abi/generated)

# PRODUCTION ABI parser pairs — compiled into mch-classify and explicitly
# registered in AbiBridge.cpp. ../ton-abi/abi/<contract>.tolk is the single
# manifest: ton-abi compiles each declaration file to JSON in its build tree,
# exports the stems as TON_ABI_PROD_STEMS and generates one pair per stem, so a
# new declaration file needs no CMake edit here.
set(ABI_PROD_PAIRS)
foreach(MCH_ABI_STEM IN LISTS TON_ABI_PROD_STEMS)
    string(REPLACE "-" "_" MCH_ABI_STEM "${MCH_ABI_STEM}")
    list(APPEND ABI_PROD_PAIRS "${TON_ABI_GENERATED_DIR}/${MCH_ABI_STEM}_gen.cpp")
endforeach()

set_source_files_properties(${ABI_PROD_PAIRS} PROPERTIES GENERATED TRUE)

add_library(mch-classify STATIC
    src/TraceAggregates.cpp
    src/BlockTree.cpp
    src/WalletRequest.cpp
    src/Value.cpp
    src/ExprRuntime.cpp
    src/Walker.cpp
    src/HostRegistry.cpp
    src/host/HostCommon.cpp
    src/host/DexPton.cpp
    src/host/DexRecords.cpp
    src/host/BlockViews.cpp
    src/host/HostAdapter.cpp
    src/host/HostEvaa.cpp
    src/host/HostDedust.cpp
    src/host/HostTonstakers.cpp
    src/host/HostNft.cpp
    src/host/HostNftSale.cpp
    src/host/HostTgbtc.cpp
    src/host/HostStonfi.cpp
    src/host/HostDedustDeposit.cpp
    src/host/HostTonco.cpp
    src/BuildRuntime.cpp
    src/BuildDriver.cpp
    src/ActionBuild.cpp
    src/ActionPayload.cpp
    src/ActionJson.cpp
    src/GhostExternal.cpp
    src/ClassifyCore.cpp
    src/SchemaTraceLoader.cpp
    src/TieredLookupSource.cpp
    src/EnginePrep.cpp
    src/MsgParse.cpp
    src/AbiBridge.cpp
    ${ABI_PROD_PAIRS}
    src/parse/PSlice.cpp
    src/parse/ParseJettons.cpp
    src/parse/ParseDns.cpp
    src/parse/ParseVesting.cpp
    ${MCH_GENERATED_SOURCES}
)
target_include_directories(mch-classify
    PUBLIC src
    # btypes_gen.h; PUBLIC because harness TUs include it too.
    PUBLIC ${MCH_GEN_DIR}
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
    # AbiBridge.cpp includes the ABI_PROD_PAIRS generated headers.
    PRIVATE ${TON_ABI_GENERATED_DIR}
)
target_compile_features(mch-classify PUBLIC cxx_std_20)

# ton-abi bridge: mch-classify -> ton-abi ONLY (ton-abi stays mch-free).
target_link_libraries(mch-classify PUBLIC tondb-scanner tdactor ton-abi msgpack-cxx)
add_dependencies(mch-classify ton_abi_generate_sources)
add_dependencies(mch-classify mch_generate_sources)

# --- mch-fixtures: the TEST-ONLY static lib.
#
# These sources serve the .lz4 fixture corpus. They must NEVER reach
# mch-classify — and therefore never reach
# ton-index-postgres, index-engine-core or ton-trace-emulator, all of which
# link the classify lib. Same discipline as ABI_BRIDGE_TEST_FIXTURES below:
# fixture sources named in one variable, referenced by exactly one target,
# with the reason stated here.
#
# FixtureLoader.cpp uses lz4 and msgpack; lz4 remains confined to this test
# library while the product classifier also uses msgpack for action payloads.
set(MCH_FIXTURE_SOURCES
    src/fixtures/FixtureLoader.cpp
    src/fixtures/FixtureLookupSource.cpp
    # Dump renderers used by the action corpus and local A/B gates.
    src/fixtures/Render.cpp
)

# lz4: Windows/vcpkg ships an lz4 CONFIG package (lz4::lz4). Linux builds have no
# such package — external/ton's BuildLZ4 builds third-party lz4 and FORCE-caches
# LZ4_LIBRARIES / LZ4_INCLUDE_DIRS; wrap those in an imported target.
if(NOT TARGET lz4::lz4)
  find_package(lz4 CONFIG QUIET)
endif()
if(NOT TARGET lz4::lz4)
  if(NOT LZ4_LIBRARIES OR NOT LZ4_INCLUDE_DIRS)
    message(FATAL_ERROR "lz4 not found: no lz4 CONFIG package and ton's LZ4_LIBRARIES/LZ4_INCLUDE_DIRS are unset")
  endif()
  add_library(lz4::lz4 UNKNOWN IMPORTED)
  set_target_properties(lz4::lz4 PROPERTIES
    IMPORTED_LOCATION "${LZ4_LIBRARIES}"
    INTERFACE_INCLUDE_DIRECTORIES "${LZ4_INCLUDE_DIRS}")
endif()

add_library(mch-fixtures STATIC ${MCH_FIXTURE_SOURCES})
target_include_directories(mch-fixtures PUBLIC src PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_features(mch-fixtures PUBLIC cxx_std_20)
target_link_libraries(mch-fixtures PUBLIC mch-classify lz4::lz4)
add_dependencies(mch-fixtures mch_generate_sources)

# --- In-process classifier for ton-trace-emulator.
# Split in two so the emulator-free side (converter / interfaces / actor) stays
# linkable without emulator_static.
add_library(mch-classification-runtime STATIC
    src/Classify.cpp
    src/emu/EmuTraceConvert.cpp
    src/DetectorInterfaceAdapter.cpp
    src/emu/EmuClassifierActor.cpp
    src/RedisActionSerialize.cpp
    src/CelldbTier2Lookup.cpp
    src/JvaultLookup.cpp
)
target_include_directories(mch-classification-runtime PUBLIC src)
target_compile_features(mch-classification-runtime PUBLIC cxx_std_20)
target_link_libraries(mch-classification-runtime PUBLIC mch-classify)

# EmuClassifierBridge.cpp is the ONLY TU that includes TraceEmulator.h.
# ton-trace-emulator-core is add_subdirectory'd after this one; link libraries
# resolve at generate time, and the core lib knows nothing of mch (no cycle).
add_library(mch-emu-bridge STATIC src/emu/EmuClassifierBridge.cpp)
target_compile_features(mch-emu-bridge PUBLIC cxx_std_20)
target_link_libraries(mch-emu-bridge PUBLIC mch-classification-runtime ton-trace-emulator-core)

# The upstream CLI/gate executable over the product boundary.
# Bridge self-test (--abi-bridge-test): build-time ton-abi generated fixture
# pairs compiled into the HARNESS EXE ONLY (never the mch-classify prod lib) so
# the trampoline+adapter run against real generated structs; these are UNDER TEST,
# not production registry rows. TON_ABI_GENERATED_DIR set above.
set(ABI_BRIDGE_TEST_FIXTURES
    # jetton_wallet_contract: the only Cell-through-the-trampoline coverage.
    # lots_of_wrappers: both union forms + address/optional/list/nested struct.
    ${TON_ABI_GENERATED_DIR}/jetton_wallet_contract_gen.cpp
    ${TON_ABI_GENERATED_DIR}/lots_of_wrappers_gen.cpp
)

set_source_files_properties(${ABI_BRIDGE_TEST_FIXTURES} PROPERTIES GENERATED TRUE)

add_custom_command(
    OUTPUT ${MCH_GEN_DIR}/expr_vectors_test_generated.cpp
    COMMAND mch-codegen --vectors ${MCH_EXPR_VECTORS}
            -o ${MCH_GEN_DIR}/expr_vectors_test_generated.cpp
    DEPENDS mch-codegen ${MCH_EXPR_VECTORS}
    COMMENT "Generate MCH expression conformance functions"
    VERBATIM
)

add_executable(ton-mch-engine
    src/main.cpp
    harness/ClassifyDump.cpp
    harness/AbiBridgeTest.cpp
    harness/RedisActionSerializeTest.cpp
    harness/CelldbTier2LookupTest.cpp
    harness/ClassifyCoreTest.cpp
    harness/ActionJsonTest.cpp
    harness/GhostExternalTest.cpp
    harness/ExprVectorsTest.cpp
    harness/WalkerCasesTest.cpp
    ${MCH_GEN_DIR}/expr_vectors_test_generated.cpp
    ${ABI_BRIDGE_TEST_FIXTURES}
)
target_include_directories(ton-mch-engine PRIVATE src harness ${CMAKE_CURRENT_BINARY_DIR} ${TON_ABI_GENERATED_DIR})
target_compile_features(ton-mch-engine PRIVATE cxx_std_20)
target_compile_definitions(ton-mch-engine PRIVATE
    MCH_ACTION_DDL="${CMAKE_CURRENT_SOURCE_DIR}/../ton-index-postgres/src/migrate.cpp"
    MCH_ACTION_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/goldens/fixtures.json"
    MCH_ACTION_TRACES="${CMAKE_CURRENT_SOURCE_DIR}/../../indexer/tests/traces"
)
# mch-classification-runtime (not just mch-classify): --actions-msgpack-test gates the write-back
# writer, which lives there. Still no emulator dependency — mch-emu-bridge is
# the only target that knows ::Trace. mch-fixtures is linked HERE and only here.
target_link_libraries(ton-mch-engine mch-fixtures mch-classify mch-classification-runtime)
add_dependencies(ton-mch-engine mch_generate_sources)
add_dependencies(ton-mch-engine mch_generate_test_vectors)

add_executable(mch-smc-pruned-cells-test harness/SmcPrunedCellsTest.cpp)
target_link_libraries(mch-smc-pruned-cells-test PRIVATE smc-envelope)
target_compile_features(mch-smc-pruned-cells-test PRIVATE cxx_std_20)
add_dependencies(ton-mch-engine mch-smc-pruned-cells-test)

# Deliberately NOT install()ed: this is the gate/dev harness, not a product
# binary, and it has no place in the production bin dir
#

# --- CTest wiring --------------------------------------------------------------
# The deterministic, self-contained gate modes as native tests (`ctest -R mch`).
enable_testing()
add_test(NAME mch_abi_bridge
    COMMAND ton-mch-engine --abi-bridge-test)
add_test(NAME mch_actions_msgpack
    COMMAND ton-mch-engine --actions-msgpack-test)
add_test(NAME mch_actions_json
    COMMAND ton-mch-engine --actions-json-test)
add_test(NAME mch_celldb_tier2
    COMMAND ton-mch-engine --celldb-tier2-test)
add_test(NAME mch_ghost_external
    COMMAND ton-mch-engine --ghost-external-test)
add_test(NAME mch_classify_core
    COMMAND ton-mch-engine --classify-core-test)
add_test(NAME mch_expr_vectors
    COMMAND ton-mch-engine --expr-vectors-test ${MCH_EXPR_VECTORS})
add_test(NAME mch_smc_pruned_cells
    COMMAND mch-smc-pruned-cells-test)
add_test(NAME mch_walker_cases
    COMMAND ton-mch-engine --walker-cases-test)
add_test(NAME mch_actions_golden
    COMMAND "${CMAKE_COMMAND}"
            -DENGINE=$<TARGET_FILE:ton-mch-engine>
            -DTRACES=${CMAKE_CURRENT_SOURCE_DIR}/../../indexer/tests/traces
            -DGOLDEN_DIR=${CMAKE_CURRENT_SOURCE_DIR}/goldens/actions
            -DFIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/goldens/fixtures.json
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/actions_golden_check
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ActionsGoldenCheck.cmake)

add_test(NAME mch_ir_equivalence
    COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${MCH_GENERATED_IR}" "${MCH_GENERATED_IR_SECOND}")
