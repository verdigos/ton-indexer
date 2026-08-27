---
module: MsgParse
source: ton-index-worker/ton-mch-engine/src/MsgParse.cpp
source: ton-index-worker/ton-mch-engine/src/MsgParse.h
source-commit: 06d4c50
---

# MsgParse

MsgParse owns the registry that maps message-type names to body-parser functions, the dispatch seam between the classifier and the per-family adapters. Two sources are merged once into a process-wide map: the handful of handwritten parsers still lacking an ABI declaration and the ABI-bridge rows compiled into AbiBridge.cpp; a key claimed by more than one source is stored with a null parser so dispatch fails closed, while validate_registries reports the offending names at startup. parse_message_body is the sanctioned dispatch path and converts the legitimate parser exception kinds (cell VM errors and std::exception subclasses) into ordinary status errors, letting infrastructure faults propagate to the trace-level handler. The registry is a Meyer's singleton: built on first use under thread-safe statics, immutable afterwards, and only ever handed out by const reference, so every accessor is an unsynchronized pure lookup.

## class mch::BuiltRegistry

- decl: `ton-index-worker/ton-mch-engine/src/MsgParse.cpp:57`

Aggregate holding the merged name-to-parser map plus the list of keys claimed by more than one source.

Built once inside the built_registry static initializer and never mutated afterwards. The map can hold null parsers for duplicated keys, and dups records those names so validate_registries can report them; consumers only ever see const references to the whole aggregate.

## duplicate_parser_keys

`std::vector<std::string> duplicate_parser_keys(const ParserSources & sources)`

- decl: `ton-index-worker/ton-mch-engine/src/MsgParse.cpp:33`
- callers: mch::built_registry (ton-index-worker/ton-mch-engine/src/MsgParse.cpp:77)

Lists every key that occurs in more than one source row list, each name once, in first-seen order.

Walks the given source vectors in order, tracking seen keys and already-reported duplicates in separate sets so a name repeated many times still appears exactly once. Pure over its argument, with an empty result when the sources are disjoint. built_registry feeds it the same source enumeration used for the merge, so the scan and the merge can never disagree on the source set or its order.

## hand_message_parsers

`const std::vector<std::pair<std::string, MsgParserFn>> & hand_message_parsers()`

- decl: `ton-index-worker/ton-mch-engine/src/MsgParse.cpp:49`
- callers: mch::built_registry (ton-index-worker/ton-mch-engine/src/MsgParse.cpp:67)

Returns the static row list of handwritten parsers that still have no ABI declaration.

Currently two rows, ChangeDnsRecordMessage and VestingSendMessage; every other parser lives in the ABI bridge. The rows sit in a function-local static, so the reference is stable and initialization is thread-safe. built_registry enumerates this list ahead of the ABI rows, and a name appearing in both is poisoned to a null parser at merge time.

## BuiltRegistry

`void BuiltRegistry()`

- decl: `ton-index-worker/ton-mch-engine/src/MsgParse.cpp:57`
- callers: mch::built_registry (ton-index-worker/ton-mch-engine/src/MsgParse.cpp:68)

Implicit default constructor; the map and duplicate list start empty.


## BuiltRegistry

`void BuiltRegistry(BuiltRegistry &&)`

- decl: `ton-index-worker/ton-mch-engine/src/MsgParse.cpp:57`
- callers: mch::built_registry (ton-index-worker/ton-mch-engine/src/MsgParse.cpp:78)

Implicit move constructor; moves the map and the duplicate-name list.


## built_registry

`const BuiltRegistry & built_registry()`

- decl: `ton-index-worker/ton-mch-engine/src/MsgParse.cpp:62`
- callers: mch::parse_message_body (ton-index-worker/ton-mch-engine/src/MsgParse.cpp:105); mch::message_parsers (ton-index-worker/ton-mch-engine/src/MsgParse.cpp:86); mch::validate_registries (ton-index-worker/ton-mch-engine/src/MsgParse.cpp:90)
- calls: `mch::(lambda at /home/verdigos/dev/ton-indexer-private/ton-index-worker/ton-mch-engine/src/MsgParse.cpp:63:38)::operator()`, `mch::BuiltRegistry::BuiltRegistry`, `mch::abi_message_parsers`, `mch::duplicate_parser_keys`, `mch::hand_message_parsers`

Returns the process-wide merged parser registry, built on first call.

A function-local static initialized from a lambda that enumerates hand_message_parsers followed by abi_message_parsers exactly once, so the merge and the duplicate scan share a single source order. Rows are emplaced into the map; when a key is already present its stored parser is set to null, so an ambiguous name fails closed instead of silently resolving to one source. Duplicate names land in dups for validate_registries. Construction is protected by C++ magic statics and the aggregate is immutable for the process lifetime; every later call just returns a const reference.

## operator()

`BuiltRegistry operator()() const`

- decl: `ton-index-worker/ton-mch-engine/src/MsgParse.cpp:63`
- callers: mch::built_registry (ton-index-worker/ton-mch-engine/src/MsgParse.cpp:63)

Builds the registry: merges both parser sources into one map, poisoning repeated keys to null, and records the duplicate names.

Enumerates the handwritten rows then the ABI rows, emplacing each pair into a fresh map; a second row with the same key overwrites that entry's parser with null rather than replacing the row. The identical ParserSources list is reused for duplicate_parser_keys, keeping the merge and the duplicate report consistent. Returns the finished aggregate by value for the enclosing static to hold.

## message_parsers

`const std::map<std::string, MsgParserFn> & message_parsers()`

- decl: `ton-index-worker/ton-mch-engine/src/MsgParse.h:27`
- callers: mch::test_abi_rows_dispatch (ton-index-worker/ton-mch-engine/harness/AbiBridgeTest.cpp:482); mch::run_surface (ton-index-worker/ton-mch-engine/harness/Surface.cpp:78); [mch::build_skip_reason (ton-index-worker/ton-mch-engine/src/BuildDriver.cpp:32)](BuildDriver.md#build_skip_reason); [mch::soft_parse (ton-index-worker/ton-mch-engine/src/BuildRuntime.cpp:34)](BuildRuntime.md#soft_parse); mch::warn_missing_artifact_parsers (ton-index-worker/ton-mch-engine/src/fixtures/Render.cpp:290)
- calls: `mch::built_registry`

Returns the merged name-to-parser map that the whole engine dispatches through.

Forwards to the built singleton's map, so it never errors and is callable from any thread at any time. Entries can hold null parsers for keys claimed by more than one source, so code iterating the map or dereferencing a value directly must null-check; parse_message_body performs that check and is the safe dispatch path. Registry problems cannot surface through this accessor by design, which is why validate_registries exists as a separate startup gate.

## validate_registries

`td::Status validate_registries()`

- decl: `ton-index-worker/ton-mch-engine/src/MsgParse.h:34`
- callers: main (ton-index-worker/ton-mch-engine/devtools/dev_main.cpp:115); mch::test_registry_policy (ton-index-worker/ton-mch-engine/harness/AbiBridgeTest.cpp:464); [mch::make_engine_prep (ton-index-worker/ton-mch-engine/src/EnginePrep.cpp:12)](EnginePrep.md#make_engine_prep); main (ton-index-worker/ton-mch-engine/src/main.cpp:70)
- calls: `mch::built_registry`

Startup check that errors when any parser key was claimed by more than one source.

Reads the duplicate names recorded when the singleton was built and joins them into one error prefixed duplicate message parser keys; returns OK when the list is empty. The check is advisory rather than a gate on map access: nothing forces it to run before message_parsers is used, and a poisoned key reached through parse_message_body produces an ambiguously-registered error instead of a crash, so the registry stays safe even unvalidated.

## parse_message_body

`td::Result<Value> parse_message_body(const std::string & type_name, const td::Ref<vm::Cell> & body)`

- decl: `ton-index-worker/ton-mch-engine/src/MsgParse.h:38`
- callers: mch::test_try_parse_first_jetton_payloads (ton-index-worker/ton-mch-engine/harness/AbiBridgeTest.cpp:198); mch::test_jetton_snake_failure_keeps_boc (ton-index-worker/ton-mch-engine/harness/AbiBridgeTest.cpp:214); mch::test_jetton_inline_copybug_regression (ton-index-worker/ton-mch-engine/harness/AbiBridgeTest.cpp:254); mch::test_vesting_add_whitelist_flattening (ton-index-worker/ton-mch-engine/harness/AbiBridgeTest.cpp:285); mch::test_vesting_add_whitelist_flattening (ton-index-worker/ton-mch-engine/harness/AbiBridgeTest.cpp:301); mch::test_registry_policy (ton-index-worker/ton-mch-engine/harness/AbiBridgeTest.cpp:467); mch::test_abi_rows_dispatch (ton-index-worker/ton-mch-engine/harness/AbiBridgeTest.cpp:488); mch::test_dedust_swap_notification_cellof (ton-index-worker/ton-mch-engine/harness/AbiBridgeTest.cpp:519); mch::test_dedust_v2_claim_reward (ton-index-worker/ton-mch-engine/harness/AbiBridgeTest.cpp:623); mch::dump_fixture (ton-index-worker/ton-mch-engine/harness/ParseDump.cpp:129); [mch::soft_parse (ton-index-worker/ton-mch-engine/src/BuildRuntime.cpp:40)](BuildRuntime.md#soft_parse); [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:102)](HostCoffee.md#coffee_swap); [mch::pton_ton_amount (ton-index-worker/ton-mch-engine/src/host/HostCommon.cpp:76)](HostCommon.md#pton_ton_amount); [mch::dedust_swap_legs (ton-index-worker/ton-mch-engine/src/host/HostDedust.cpp:147)](HostDedust.md#dedust_swap_legs); [mch::dedust_swap_legs (ton-index-worker/ton-mch-engine/src/host/HostDedust.cpp:279)](HostDedust.md#dedust_swap_legs); [mch::dedust_deposit_final_data (ton-index-worker/ton-mch-engine/src/host/HostDedustDeposit.cpp:204)](HostDedustDeposit.md#dedust_deposit_final_data); [mch::dedust_deposit_final_data (ton-index-worker/ton-mch-engine/src/host/HostDedustDeposit.cpp:249)](HostDedustDeposit.md#dedust_deposit_final_data); [mch::dedust_deposit_partial_data (ton-index-worker/ton-mch-engine/src/host/HostDedustDeposit.cpp:434)](HostDedustDeposit.md#dedust_deposit_partial_data); [mch::parse_pool_deposit (ton-index-worker/ton-mch-engine/src/host/HostDedustDeposit.cpp:95)](HostDedustDeposit.md#parse_pool_deposit); [mch::evaa_supply_data (ton-index-worker/ton-mch-engine/src/host/HostEvaa.cpp:308)](HostEvaa.md#evaa_supply_data); [mch::evaa_supply_data (ton-index-worker/ton-mch-engine/src/host/HostEvaa.cpp:374)](HostEvaa.md#evaa_supply_data); [mch::parse_ownership_prev_owner (ton-index-worker/ton-mch-engine/src/host/HostNft.cpp:110)](HostNft.md#parse_ownership_prev_owner); [mch::parse_pay_to (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:115)](HostStonfi.md#parse_pay_to); [mch::pton_self_transfer (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:498)](HostStonfi.md#pton_self_transfer); [mch::parse_payment_request (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:60)](HostStonfi.md#parse_payment_request); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:707)](HostStonfi.md#stonfi_v2_swap_data); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:820)](HostStonfi.md#stonfi_v2_swap_data); [mch::parse_swap_v1 (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:89)](HostStonfi.md#parse_swap_v1); [mch::tonco_deposit_liquidity_data (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:267)](HostTonco.md#tonco_deposit_liquidity_data); [mch::parse_v3_swap_source_wallet (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:52)](HostTonco.md#parse_v3_swap_source_wallet); [mch::tonco_swap_data (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:658)](HostTonco.md#tonco_swap_data)
- calls: `mch::built_registry`

Parses a message body cell with the parser registered under the given type name.

An unknown name yields a not-registered error, and a key poisoned at merge time yields an ambiguously-registered error; both are ordinary results callers build soft-parse fallbacks on. The registered parser runs under a try/catch that converts vm::VmError, VmVirtError, VmNoGas and std::exception into parse-failure statuses with a parse threw prefix, so a malformed body is a clean mismatch. Anything else, such as bad_alloc or unknown host faults, propagates untouched so the trace-level handler sees an infrastructure failure.

## ton_transfer_comment

`std::optional<std::string> ton_transfer_comment(const td::Ref<vm::Cell> & body)`

- decl: `ton-index-worker/ton-mch-engine/src/MsgParse.h:43`
- callers: [mch::leaf_comment_info (ton-index-worker/ton-mch-engine/src/BlockTree.cpp:49)](BlockTree.md#leaf_comment_info)
- calls: `mch::decode_backslashreplace_strip_nul`, `mch::load_snake_bytes`, `mch::open_body`

Extracts the comment from a plain TON transfer body, nullopt when absent or malformed.

Opens the body and requires at least 32 bits so an opcode exists; fewer bits mean no comment. The remainder must be byte-aligned with at most one ref per cell and must load as a snake string, and every failure along that path, from a null or bad body cell to snake-load errors, comes back as nullopt rather than an error. Opcode 0x2167da4b marks an encrypted comment, returned base64-encoded; plain comments go through the utf-8 backslashreplace codec that also strips U+0000.

## decode_comment_bytes

`std::string decode_comment_bytes(const std::string & raw)`

- decl: `ton-index-worker/ton-mch-engine/src/MsgParse.h:48`
- callers: [mch::comment_value (ton-index-worker/ton-mch-engine/src/ActionBuild.cpp:72)](ActionBuild.md#comment_value)
- calls: `mch::decode_backslashreplace_strip_nul`

Decodes raw jetton-transfer and NFT comment payload bytes with the transfer-comment codec.

Thin forwarder to decode_backslashreplace_strip_nul, the utf-8 backslashreplace codec with U+0000 stripped, so payload comments stay byte-compatible with plain transfer comments.
