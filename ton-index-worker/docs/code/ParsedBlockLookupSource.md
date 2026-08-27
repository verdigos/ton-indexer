---
module: ParsedBlockLookupSource
source: ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp
source: ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.h
source-commit: d508a02
---

# ParsedBlockLookupSource

ParsedBlockLookupSource is the LookupSource classify_schema_trace evaluates lookups against: tier 1 is the map of interfaces already computed for accounts touched in the block (account_interfaces_), tier 2 an optional injectable hook (Tier2Hook, typically EmuCelldbTier2::fetch) consulted on every tier-1 miss. fetch() tries tier1() first and returns the hook's answer — or null with no hook — so unresolved calls are misses, never exceptions; malformed addresses and missing accounts are clean misses too. fetch() is const, so per-tier attribution lives in a mutable LookupStats (tier1_hits/tier2_hits/misses plus by-kind maps whose values sum to their totals exactly) that is telemetry only and never changes a returned Value. Instances live for one classify call on one thread: the interface map is a borrowed pointer that must outlive the source, and the hook's captures are single-call state.

## class mch::ParsedBlockLookupSource

- decl: `ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.h:18`

Two-tier LookupSource: block-local interface map first, optional tier-2 fallback hook second.

Constructed per classify call over a borrowed InterfaceMap pointer and an optional Tier2Hook; neither is owned, and hook captures (e.g. a stack EmuCelldbTier2) must outlive the source. All misses are nulls, never throws — a bad lookup cannot fail a trace from here. stats() is read after classification to fold per-source counters into run totals.

## class mch::ParsedBlockLookupSource::LookupStats

- decl: `ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.h:36`

Plain per-source counters: tier-1/tier-2 hits and misses, with per-kind attribution for the tier-2 hits and the misses.


## addr_str

`Value addr_str(const block::StdAddress & a)`

- decl: `ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:18`
- callers: mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:107); mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:124); mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:125); mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:134); mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:135); mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:147); mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:148); mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:175); mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:183); mch::addr_str_opt (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:24); mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:96); mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:97); mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:98)
- calls: `convert::to_raw_address`, `mch::Value::make_str`, `mch::canonicalize_or_passthrough`

Renders a StdAddress as the canonical uppercase raw "wc:HEX" string.

convert::to_raw_address plus canonicalize_or_passthrough — the same normalization SchemaTraceLoader applies to trace accounts and the fixtures store, so interface addresses compare equal to trace addresses by text.

## tier1

`Value tier1(const std::string & kind, const std::string & addr) const`

- decl: `ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:194`
- callers: mch::ParsedBlockLookupSource::fetch (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:228)
- calls: `mch::ParsedBlockLookupSource::iface_value`, `mch::Value::Value`, `mch::Value::is_null`, `mch::Value::null`

Tier-1 resolve: look the address up in the interface map and return the first interface that answers this kind.

A null map pointer, malformed address, or absent account is a null miss; an account present with no variant matching the kind is also a miss, routing fetch() to tier 2. There is deliberately no kind whitelist here — iface_value's per-branch kind tests self-gate, and a second hand-kept list could only drift out of sync with them.

## fetch

`Value fetch(const std::string & kind, const std::vector<Value> & args) const`

- decl: `ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:215`
- callers: mch::via_tier1 (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:104); mch::test_two_tier_routing (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:454); mch::test_two_tier_routing (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:458); mch::test_two_tier_routing (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:469); mch::GateProdLookupSource::fetch (ton-index-worker/ton-mch-engine/harness/SchemaGate.cpp:259)
- calls: `mch::ParsedBlockLookupSource::tier1`, `mch::Value::Value`, `mch::Value::is_null`, `mch::Value::null`

Lookup entry point: validate arguments, try tier 1, then the tier-2 hook, charging stats to whichever tier served.

Accepts exactly one Str or non-null Account argument (FixtureLookupSource parity); anything else is a null result on both tiers. A tier-1 hit returns directly; a miss calls tier2_ when present (null otherwise), with the outcome incrementing tier2_hits (+by kind) or misses (+by kind). fetch() is const — stats_ is mutable telemetry only and never affects the returned Value.

## addr_str_opt

`Value addr_str_opt(const std::optional<block::StdAddress> & a)`

- decl: `ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:23`
- callers: mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:111); mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:112); mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:128); mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:138); mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:151); mch::dedust_asset (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:59)
- calls: `mch::Value::null`, `mch::addr_str`

Optional-address renderer: addr_str when present, null for nullopt.


## code_hash_str

`Value code_hash_str(const td::Bits256 & h)`

- decl: `ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:32`
- callers: mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:114); mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:129); mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:139); mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:152)
- calls: `mch::Value::make_str`, `mch::Value::null`

Renders a code hash as base64, or null when the Bits256 is all-zero.

An all-zero hash is the emulator adapter's value-initialized default, never a real observed hash; null keeps it a miss readers can reject on, instead of a bogus b64 string silently matching a literal.

## nft_content

`Value nft_content(const std::optional<std::map<std::string, std::string>> & content)`

- decl: `ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:43`
- callers: mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:113)
- calls: `mch::Value::make_dict`, `mch::Value::make_str`, `mch::Value::null`

Renders NFT item content as a flat Dict of string fields, or null when content is absent.

It must be a Dict because get_nft_data tests the top-level uri key and faults on any other type; nested metadata is never read by matchers. Missing content stays null so the lookup rejects rather than fabricating an empty record.

## dedust_asset

`Value dedust_asset(const std::optional<block::StdAddress> & a)`

- decl: `ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:56`
- callers: mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:161); mch::ParsedBlockLookupSource::iface_value (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:162)
- calls: `mch::Value::make_bool`, `mch::Value::make_obj`, `mch::addr_str_opt`

Renders a DeDust pool asset slot as {is_ton, address}; a nullopt slot is the TON side.


## iface_value

`Value iface_value(const std::string & kind, const schema::BlockchainInterfaceV2 & iface)`

- decl: `ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:65`
- callers: mch::test_tier_shapes (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:114); mch::test_tier_shapes (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:138); mch::test_tier_shapes (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:178); mch::test_tier_shapes (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:193); mch::test_tier_shapes (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:204); mch::test_two_tier_routing (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:450); mch::ParsedBlockLookupSource::tier1 (ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:207); [mch::EmuCelldbTier2::resolve (ton-index-worker/ton-mch-engine/src/emu/EmuCelldbLookup.cpp:125)](EmuCelldbLookup.md#resolve)
- calls: `mch::Value::make_bool`, `mch::Value::make_int`, `mch::Value::make_list`, `mch::Value::make_obj`, `mch::Value::null`, `mch::addr_str`, `mch::addr_str_opt`, `mch::code_hash_str`, `mch::dedust_asset`, `mch::nft_content`

The single V2-interface-to-Value shape mapper, shared by tier 1 and tier 2 alike.

Dispatches kind to handler to interface variant; an unknown kind or a variant that doesn't match returns null. EmuCelldbTier2::resolve renders its detector output through this same static function, so the two tiers are shape-identical by construction rather than by review. NominatorPool renders only {address} — the one spec reading it does set membership and never touches a field.


## ParsedBlockLookupSource

`void ParsedBlockLookupSource(const InterfaceMap * account_interfaces, Tier2Hook tier2)`

- decl: `ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.h:26`
- callers: mch::via_tier1 (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:103); mch::test_two_tier_routing (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:452); mch::classify_txs (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:66); [BatchRunner::classify_trace (ton-index-worker/ton-mch-index/src/BatchRunner.cpp:238)](BatchRunner.md#classify_trace)

Stores the borrowed interface-map pointer and the optional tier-2 hook.


## operator=

`LookupStats & operator=(const LookupStats &)`

- decl: `ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.h:36`
- callers: mch::classify_txs (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:74)

Copy-assigns the counters; how callers fold per-call stats into run totals.


## stats

`const LookupStats & stats() const`

- decl: `ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.h:48`
- callers: mch::test_two_tier_routing (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:456); mch::test_two_tier_routing (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:460); mch::test_two_tier_routing (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:462); mch::test_two_tier_routing (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:471); mch::test_two_tier_routing (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:472); mch::classify_txs (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:74); [BatchRunner::classify_trace (ton-index-worker/ton-mch-index/src/BatchRunner.cpp:275)](BatchRunner.md#classify_trace)

Accessor for the accumulated per-source lookup counters.

