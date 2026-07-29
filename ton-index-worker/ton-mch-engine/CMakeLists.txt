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
# postgres links THIS; the CLI exe below links it + the harness TUs. Action-row
# construction (Action struct + build_action + the fill_*) lives here in
# ActionBuild.cpp; only the dump-side render_action + stdout printing stayed
# with the harness Classify.cpp.
# ton-abi generated pairs live here (fixture pairs for the harness test below;
# the PRODUCTION pairs compiled into mch-classify).
set(TON_ABI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ton-abi)

# PRODUCTION ABI parser pairs — compiled into mch-classify, their rows
# registered in AbiBridge.cpp abi_message_parsers(). Adding protocol #2 that
# needs a NEW .tolk decl file = one line here (a new decl in an EXISTING
# contract needs no CMake edit at all; see ton-abi/ABI-PROTOCOL-CHECKLIST.md).
set(ABI_PROD_PAIRS
    # T12.2: DeDust CPMM V2 claim_reward family.
    ${TON_ABI_DIR}/generated/dedust_v2_gen.cpp
)

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
    src/host/HostVesting.cpp
    src/host/HostJvault.cpp
    src/host/HostNft.cpp
    src/host/HostTgbtc.cpp
    src/host/HostCoffee.cpp
    src/host/HostStonfi.cpp
    src/host/HostDedustDeposit.cpp
    src/host/HostTonco.cpp
    src/BuildRuntime.cpp
    src/BuildDriver.cpp
    src/ActionBuild.cpp
    src/ClassifyCore.cpp
    src/ClassifyDriver.cpp
    src/KvrocksDecode.cpp
    src/SchemaTraceLoader.cpp
    src/ParsedBlockLookupSource.cpp
    src/MchClassifierActor.cpp
    src/MsgParse.cpp
    src/MsgParsersGenerated.cpp
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
    # MsgParse.cpp includes "generated/mch-msgs-tlb.h" -> resolves into the
    # build tree; nothing generated ever lands under src/.
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
    # AbiBridge.cpp includes the ABI_PROD_PAIRS generated headers.
    PRIVATE ${TON_ABI_DIR}/generated
)
target_compile_features(mch-classify PUBLIC cxx_std_20)

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
# ton-abi (W12 bridge): mch-classify -> ton-abi ONLY (ton-abi stays mch-free).
target_link_libraries(mch-classify PUBLIC tondb-scanner tdactor lz4::lz4 ton-abi)
add_dependencies(mch-classify mch_generate_sources)

# --- EM1 in-process shadow classifier for ton-trace-emulator (em1-design §8).
# Split in two so the emulator-free side (converter / actor / capture) stays
# linkable without emulator_static — a later replay harness links mch-emu only.
add_library(mch-emu STATIC
    src/emu/EmuTraceConvert.cpp
    src/emu/EmuInterfaces.cpp
    src/emu/EmuClassifierActor.cpp
    src/emu/EmuLookupActor.cpp
    src/emu/EmuCapture.cpp
    # EM2 W1: the mch::Action -> msgpack write-back writer
    # (em2-writeback-design.md §2). Pure library function, no actor and no
    # Redis; W3 calls it from EmuClassifierActor, the harness self-test below
    # and W2's twin gate call it directly.
    src/emu/EmuActionSerialize.cpp
)
target_include_directories(mch-emu PUBLIC src)
target_compile_features(mch-emu PUBLIC cxx_std_20)
# ton-kvrocks-state: EmuLookupActor reuses its KvrocksConfig/KvrocksClient (and
# hence its Sentinel support and connection building) for the tier-2 transport.
target_link_libraries(mch-emu PUBLIC mch-classify ton-kvrocks-state)

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
    src/Classify.cpp
    src/ShapeTest.cpp
    src/DexRecordsTest.cpp
    src/EmuActionSerializeTest.cpp
    src/MsgVectorRunner.cpp
    src/ParseDump.cpp
    src/LookupKeyGate.cpp
    src/KvrocksDecodeTest.cpp
    src/SchemaGate.cpp
    src/Surface.cpp
    src/Tier2SuspendTest.cpp
    src/AbiBridgeTest.cpp
    ${ABI_BRIDGE_TEST_FIXTURES}
)
target_include_directories(ton-mch-engine PRIVATE src ${CMAKE_CURRENT_BINARY_DIR} ${TON_ABI_DIR}/generated)
target_compile_features(ton-mch-engine PRIVATE cxx_std_20)
# mch-emu (not just mch-classify): --actions-msgpack-test gates the EM2 writer,
# which lives there. Still no emulator dependency — mch-emu-bridge is the only
# TU that knows TraceEmulator.h.
# ton-kvrocks-state: --kvrocks-decode-test generates its payload vectors with the
# interface writer's OWN build_payload, so the tier-2 decoder is gated against the
# real wire format rather than a copy of it.
target_link_libraries(ton-mch-engine mch-classify mch-emu ton-kvrocks-state)
add_dependencies(ton-mch-engine mch_generate_sources)

install(TARGETS ton-mch-engine RUNTIME DESTINATION bin)

# --- mch-ab-replay: EM2 phase-1 A/B replay adapter (em2-writeback-design.md §8,
# wave W7 item iii). Reads a read-only prod-Redis HGETALL sweep (JSONL,
# twins/README.md) and emits the `--actions-msgpack` line format, so
# twins/ab_replay_diff.py compares Python's stored `actions` against ours with no
# new comparator code. Offline: no Redis client, no DB, no emulator run.
#
# A separate exe rather than another ton-mch-engine mode BECAUSE of the link edge
# below — only this binary needs the emulator's wire structs, and the fixture
# harness should not grow a dependency on emulator_static.
add_executable(mch-ab-replay
    src/abreplay/main.cpp
    src/abreplay/RedisReplay.cpp
)
target_include_directories(mch-ab-replay PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})
target_compile_features(mch-ab-replay PRIVATE cxx_std_20)
# MSGPACK_USE_DEFINE_MAP — which makes every MSGPACK_DEFINE struct in Redis a MAP
# keyed by field name rather than a positional array — is INHERITED from
# ton-trace-emulator-core, the same target that supplies Serializer.hpp's include
# path. That pairing is deliberate (see the definition site): a reader cannot
# obtain the header without also obtaining the format it is written in. Do not
# re-add the definition here.
# ton-trace-emulator-core carries Serializer.hpp's include path (and its
# TraceEmulator.h prerequisite). Same edge mch-emu-bridge has, and it resolves at
# generate time even though ton-trace-emulator is add_subdirectory'd after this.
# RedisReplay.cpp is the only TU here that includes either header, and it READS
# them — nothing under ton-trace-emulator/ or src/emu/ is modified.
target_link_libraries(mch-ab-replay mch-classify mch-emu ton-trace-emulator-core msgpack-cxx)
add_dependencies(mch-ab-replay mch_generate_sources)

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
add_test(NAME mch_msg_vectors
    COMMAND ton-mch-engine --msg-vectors ${CMAKE_CURRENT_SOURCE_DIR}/ir/msg_vectors.json)
add_test(NAME mch_codegen_selftest
    COMMAND mch-codegen --selftest)
add_test(NAME mch_abi_bridge
    COMMAND ton-mch-engine --abi-bridge-test)
add_test(NAME mch_kvrocks_decode
    COMMAND ton-mch-engine --kvrocks-decode-test)
add_test(NAME mch_actions_msgpack
    COMMAND ton-mch-engine --actions-msgpack-test)
# S7 (W4): the tier-2 suspension loop, driven on a real scheduler with a fake
# fetch. Hermetic like the rest of this set — no store, no artifact, no corpus —
# but it WAITS on the sweep, so it is the one test here that takes seconds
# rather than milliseconds.
add_test(NAME mch_tier2_suspend
    COMMAND ton-mch-engine --tier2-suspend-test)
