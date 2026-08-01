cmake_minimum_required(VERSION 3.16)

# --- mch-ir-load: the IR artifact PARSER, alone in a target because two very
# different things need it and NEITHER of them is production:
#   - mch-codegen (below), which compiles the artifact into the matcher tables;
#   - the harness exe, whose mch_ir_equivalence ctest uses it as the reference
#     the generated table is compared against.
# It links tdutils and nothing else — that is what lets the build-time tool use
# it, and it is why the runnable/skip decision (which needs the host registry)
# lives in BuildDriver.cpp instead of in the loader. mch-classify does NOT link
# it: no production binary parses an IR document any more.
# TODO: remove (harness): with the tables compiled in, the last thing keeping the
# parser alive is the equivalence gate.
add_library(mch-ir-load STATIC src/fixtures/IrLoader.cpp)
target_include_directories(mch-ir-load PUBLIC src)
target_compile_features(mch-ir-load PUBLIC cxx_std_20)
target_link_libraries(mch-ir-load PUBLIC tdutils)

# --- mch-codegen: build-time IR -> C++ source generator (no Python in the build).
# Reads the version-controlled IR JSON under ir/ and emits deterministic C++.
add_executable(mch-codegen
    codegen/main.cpp
    codegen/ExprCodegen.cpp
    codegen/MatcherCodegen.cpp
)
target_compile_features(mch-codegen PRIVATE cxx_std_20)
target_link_libraries(mch-codegen tdutils mch-ir-load)

# --- generated sources ---------------------------------------------------------
# EVERY generated file (mch-codegen tables + tlbc parsers) is a build step whose
# output lives in the build tree (${CMAKE_CURRENT_BINARY_DIR}/generated/), never
# in the source tree or git. The ir/*.json inputs and tlb/mch-msgs.tlb ARE the
# version-controlled contract; the .cpp tables are derived artifacts.
#
# Per-document tables: mch-codegen emits gen_*_<suffix>() symbols, so the tables
# generated from the CLASSIFY artifact and the one from ir/build_vectors.json
# (--build-vectors) both link into the binary, each permanently paired with its
# input document.
set(MCH_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${MCH_GEN_DIR})

# The document the CLASSIFY tables (matchers + builds + wheres) are generated
# from. -DMCH_TESTBED_IR=<artifact.json> lets a phase run classify against an
# experimental artifact WITHOUT editing the ir/mch_ir.json contract; the table
# SYMBOLS are unchanged, so swapping the document is all the knob does and no
# run-time table selection is needed. All three tables come from this one
# variable, which is why they cannot disagree unless a build is left half-done
# (prepare_classify cross-checks their source shas for exactly that case).
set(MCH_TESTBED_IR "" CACHE FILEPATH
    "Optional experimental IR artifact the classify tables are generated from instead of ir/mch_ir.json")
if(MCH_TESTBED_IR)
    set(MCH_CLASSIFY_ARTIFACT ${MCH_TESTBED_IR})
    message(STATUS "mch: classify tables from TESTBED artifact ${MCH_TESTBED_IR}")
else()
    set(MCH_CLASSIFY_ARTIFACT ${CMAKE_CURRENT_SOURCE_DIR}/ir/mch_ir.json)
endif()

add_custom_command(
    OUTPUT ${MCH_GEN_DIR}/vectors_generated.cpp
    COMMAND mch-codegen --vectors ${CMAKE_CURRENT_SOURCE_DIR}/ir/expr_vectors.json
            -o ${MCH_GEN_DIR}/vectors_generated.cpp
    DEPENDS mch-codegen ${CMAKE_CURRENT_SOURCE_DIR}/ir/expr_vectors.json
    COMMENT "Generate MCH expression conformance functions (mch-codegen --vectors)"
)
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
    OUTPUT ${MCH_GEN_DIR}/matchers_ir_generated.cpp
    COMMAND mch-codegen --matchers ${MCH_CLASSIFY_ARTIFACT}
            --suffix ir -o ${MCH_GEN_DIR}/matchers_ir_generated.cpp
    DEPENDS mch-codegen ${MCH_CLASSIFY_ARTIFACT}
    COMMENT "Generate the MCH matcher table from the IR artifact (mch-codegen --matchers)"
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

# The generated tables split by who may link them. PRODUCT: the matcher, build
# and where tables the classify loop runs, plus the TL-B parsers. TEST: the
# conformance-vector tables — their gen_* symbols are
# referenced only from the harness runners (VectorRunner / BuildVectorRunner), so
# they belong in mch-fixtures, not in the lib ton-index-postgres links
# (python-parity-audit §5.7 H3).
set(MCH_GENERATED_SOURCES
    ${MCH_GEN_DIR}/matchers_ir_generated.cpp
    ${MCH_GEN_DIR}/builds_ir_generated.cpp
    ${MCH_GEN_DIR}/wheres_ir_generated.cpp
    ${MCH_GEN_DIR}/mch-msgs-tlb.cpp
)
set(MCH_GENERATED_TEST_SOURCES
    ${MCH_GEN_DIR}/vectors_generated.cpp
    ${MCH_GEN_DIR}/builds_vectors_generated.cpp
)
add_custom_target(mch_generate_sources
    DEPENDS ${MCH_GENERATED_SOURCES} ${MCH_GENERATED_TEST_SOURCES} ${MCH_GEN_DIR}/mch-msgs-tlb.h)

# --- mch-classify: the classification CORE static library (Phase G step 3,
# prod-design §2). The pure pipeline TUs (no CLI/dump/harness) — ton-index-
# postgres links THIS; the CLI exe below links it + the harness TUs. Action-row
# construction (Action struct + build_action + the fill_*) lives here in
# ActionBuild.cpp; only the dump-side render_action + stdout printing stayed
# with the harness Classify.cpp.
# ton-abi generated pairs live here (fixture pairs for the harness test below;
# the PRODUCTION pairs compiled into mch-classify).
set(TON_ABI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ton-abi)

# PRODUCTION ABI parser pairs — compiled into mch-classify and explicitly
# registered in AbiBridge.cpp. Adding a NEW .tolk declaration file = one line here
# (a new decl in an EXISTING contract needs no CMake edit at all; see
# ton-abi/ABI-PROTOCOL-CHECKLIST.md).
set(ABI_PROD_PAIRS
    ${TON_ABI_DIR}/generated/jetton_gen.cpp
    ${TON_ABI_DIR}/generated/multisig_gen.cpp
    ${TON_ABI_DIR}/generated/pton_gen.cpp
    ${TON_ABI_DIR}/generated/stonfi_gen.cpp
    ${TON_ABI_DIR}/generated/dedust_gen.cpp
    # T12.2: DeDust CPMM V2 claim_reward family.
    ${TON_ABI_DIR}/generated/dedust_v2_gen.cpp
    ${TON_ABI_DIR}/generated/coffee_gen.cpp
    ${TON_ABI_DIR}/generated/coffee_staking_withdraw3_gen.cpp
    ${TON_ABI_DIR}/generated/evaa_gen.cpp
    ${TON_ABI_DIR}/generated/evaa_supply_forward_gen.cpp
    ${TON_ABI_DIR}/generated/jvault_gen.cpp
    ${TON_ABI_DIR}/generated/jvault_payload_gen.cpp
    ${TON_ABI_DIR}/generated/subscriptions_gen.cpp
    ${TON_ABI_DIR}/generated/tonco_gen.cpp
    ${TON_ABI_DIR}/generated/teleitem_gen.cpp
    ${TON_ABI_DIR}/generated/nft_sale_gen.cpp
    ${TON_ABI_DIR}/generated/tonstakers_gen.cpp
    ${TON_ABI_DIR}/generated/cocoon_gen.cpp
)

add_library(mch-classify STATIC
    src/TraceAggregates.cpp
    src/BlockTree.cpp
    src/Value.cpp
    src/ExprRuntime.cpp
    src/Walker.cpp
    src/HostRegistry.cpp
    src/host/HostCommon.cpp
    src/host/DexPton.cpp
    src/host/DexRecords.cpp
    src/host/BlockViews.cpp
    src/host/HostAdapter.cpp
    src/host/HostGetgems.cpp
    src/host/HostNominator.cpp
    src/host/HostEvaa.cpp
    src/host/HostDedust.cpp
    src/host/HostLayerZero.cpp
    src/host/HostTonstakers.cpp
    src/host/HostDns.cpp
    src/host/HostVesting.cpp
    src/host/HostCocoon.cpp
    src/host/HostJvault.cpp
    src/host/HostNft.cpp
    src/host/HostNftSale.cpp
    src/host/HostTgbtc.cpp
    src/host/HostCoffee.cpp
    src/host/HostStonfi.cpp
    src/host/HostDedustDeposit.cpp
    src/host/HostTonco.cpp
    src/BuildRuntime.cpp
    src/BuildDriver.cpp
    src/ActionBuild.cpp
    src/GhostExternal.cpp
    src/ClassifyCore.cpp
    src/SchemaTraceLoader.cpp
    src/ParsedBlockLookupSource.cpp
    src/EnginePrep.cpp
    src/MsgParse.cpp
    src/AbiBridge.cpp
    ${ABI_PROD_PAIRS}
    src/parse/PSlice.cpp
    src/parse/ParseJettons.cpp
    src/parse/ParseDns.cpp
    src/parse/ParseVesting.cpp
    src/parse/ParseDedust.cpp
    src/parse/ParseEvaa.cpp
    src/parse/ParseLayerZero.cpp
    ${MCH_GENERATED_SOURCES}
)
target_include_directories(mch-classify
    PUBLIC src
    # Kept-hand parser TUs include "generated/mch-msgs-tlb.h" -> resolves into
    # the build tree; nothing generated ever lands under src/.
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
    # AbiBridge.cpp includes the ABI_PROD_PAIRS generated headers.
    PRIVATE ${TON_ABI_DIR}/generated
)
target_compile_features(mch-classify PUBLIC cxx_std_20)

# ton-abi (W12 bridge): mch-classify -> ton-abi ONLY (ton-abi stays mch-free).
target_link_libraries(mch-classify PUBLIC tondb-scanner tdactor ton-abi)
add_dependencies(mch-classify mch_generate_sources)

# --- mch-fixtures: the TEST-ONLY static lib (python-parity-audit §5.1).
#
# These sources exist to serve the .lz4 fixture corpus and the conformance
# vectors. They must NEVER reach mch-classify — and therefore never reach
# ton-index-postgres, index-engine-core or ton-trace-emulator, all of which
# link the classify lib. Same discipline as ABI_BRIDGE_TEST_FIXTURES below:
# fixture sources named in one variable, referenced by exactly one target,
# with the reason stated here.
#
# lz4 and msgpack are used by FixtureLoader.cpp and by nothing else in the
# tree, so the lz4 link edge lives here rather than on the product lib.
set(MCH_FIXTURE_SOURCES
    src/fixtures/FixtureLoader.cpp
    src/fixtures/FixtureLookupSource.cpp
    # Dump renderers + the vector comparator: only the A/B gates render a Value,
    # a block key or a cellhash, or compare two Values structurally. See the
    # header for the full argument.
    src/fixtures/Render.cpp
    # The expression INTERPRETER. It moved here when the generated where table
    # became the walker's only evaluator: its sole remaining caller is the
    # mch_expr_vectors ctest, which runs it as the differential reference for the
    # emitter (mch_expr_vectors_gen runs the same vectors through generated code).
    src/fixtures/ExprEval.cpp
    ${MCH_GENERATED_TEST_SOURCES}
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
# mch-ir-load rides in HERE, not on mch-classify: the artifact parser is the
# equivalence gate's reference, not a production code path.
target_link_libraries(mch-fixtures PUBLIC mch-classify mch-ir-load lz4::lz4)
add_dependencies(mch-fixtures mch_generate_sources)

# --- In-process shadow classifier for ton-trace-emulator (em1-design §8).
# Split in two so the emulator-free side (converter / interfaces / actor) stays
# linkable without emulator_static.
add_library(mch-emu STATIC
    src/emu/EmuTraceConvert.cpp
    src/emu/EmuInterfaces.cpp
    src/emu/EmuClassifierActor.cpp
    src/emu/EmuActionSerialize.cpp
    src/emu/EmuCelldbLookup.cpp
    src/emu/EmuJvaultChain.cpp
)
target_include_directories(mch-emu PUBLIC src)
target_compile_features(mch-emu PUBLIC cxx_std_20)
target_link_libraries(mch-emu PUBLIC mch-classify)

# EmuShadow.cpp is the ONLY TU that includes TraceEmulator.h.
# ton-trace-emulator-core is add_subdirectory'd after this one; link libraries
# resolve at generate time, and the core lib knows nothing of mch (no cycle).
add_library(mch-emu-bridge STATIC src/emu/EmuShadow.cpp)
target_compile_features(mch-emu-bridge PUBLIC cxx_std_20)
target_link_libraries(mch-emu-bridge PUBLIC mch-emu ton-trace-emulator-core)

# The CLI/dev tool: harness TUs (dump/runners/main) over the core lib.
# W12 bridge self-test (--abi-bridge-test): committed ton-abi generated fixture
# pairs compiled into the HARNESS EXE ONLY (never the mch-classify prod lib) so
# the trampoline+adapter run against real generated structs; these are UNDER TEST,
# not production registry rows (design §6/§7). TON_ABI_DIR set above.
set(ABI_BRIDGE_TEST_FIXTURES
    # jetton_wallet_contract: the only Cell-through-the-trampoline coverage.
    # lots_of_wrappers: both union forms + address/optional/list/nested struct.
    ${TON_ABI_DIR}/generated/jetton_wallet_contract_gen.cpp
    ${TON_ABI_DIR}/generated/lots_of_wrappers_gen.cpp
)

add_executable(ton-mch-engine
    src/main.cpp
    src/VectorRunner.cpp
    src/MatchRunner.cpp
    src/BuildVectorRunner.cpp
    src/BuildSmoke.cpp
    src/IrEquivalence.cpp
    src/Classify.cpp
    src/ShapeTest.cpp
    src/DexRecordsTest.cpp
    src/ParseDump.cpp
    src/SchemaGate.cpp
    src/Surface.cpp
    src/AbiBridgeTest.cpp
    src/EmuActionSerializeTest.cpp
    src/EmuCelldbLookupTest.cpp
    ${ABI_BRIDGE_TEST_FIXTURES}
)
target_include_directories(ton-mch-engine PRIVATE src ${CMAKE_CURRENT_BINARY_DIR} ${TON_ABI_DIR}/generated)
target_compile_features(ton-mch-engine PRIVATE cxx_std_20)
# mch-emu (not just mch-classify): --actions-msgpack-test gates the write-back
# writer, which lives there. Still no emulator dependency — mch-emu-bridge is
# the only target that knows ::Trace. mch-fixtures is linked HERE and only here.
target_link_libraries(ton-mch-engine mch-fixtures mch-classify mch-emu)
add_dependencies(ton-mch-engine mch_generate_sources)

# Deliberately NOT install()ed: this is the gate/dev harness, not a product
# binary, and it has no place in the production bin dir
# (python-parity-audit §5.7 H35).

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
add_test(NAME mch_dexrecords
    COMMAND ton-mch-engine --dexrecords-test)
add_test(NAME mch_codegen_selftest
    COMMAND mch-codegen --selftest)
add_test(NAME mch_abi_bridge
    COMMAND ton-mch-engine --abi-bridge-test)
add_test(NAME mch_actions_msgpack
    COMMAND ton-mch-engine --actions-msgpack-test)
add_test(NAME mch_celldb_tier2
    COMMAND ton-mch-engine --celldb-tier2-test)
# The compiled-in matcher table vs the artifact parsed at run time: the gate that
# lets production stop parsing the document (src/IrEquivalence.h).
add_test(NAME mch_ir_equivalence
    COMMAND ton-mch-engine --ir-equivalence ${MCH_CLASSIFY_ARTIFACT})
