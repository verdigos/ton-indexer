---
module: HostRegistry
source: ton-index-worker/ton-mch-engine/src/HostRegistry.cpp
source: ton-index-worker/ton-mch-engine/src/HostRegistry.h
source-commit: 06d4c50
---

# HostRegistry

HostRegistry owns the three host-binding registries — predicates, fns and shapers — that matcher IR references by bare name, plus the runtime dispatch for fns. The per-protocol implementations live under src/host; this module only owns the name-to-function maps and the calling convention. All three maps are function-local statics built once and immutable afterwards, so unsynchronized concurrent reads from classify workers are safe. The IR loader gates matcher runnability on these registries: a matcher is runnable iff every predicate and fn name it references is registered, and missing names become its skip reason. rt_call_hostfn is the single fn dispatch point, where unknown names and arity mismatches become clean match rejections rather than aborts.

## class mch::HostFnEntry

- decl: `ton-index-worker/ton-mch-engine/src/HostRegistry.h:31`

A host fn callable paired with its expected argument count, where -1 defers arity checking to the implementation.

Entries are static data inside the host_fns map; when arity is non-negative, rt_call_hostfn validates the argument count before invoking the fn. Plain value pair with no ownership or state.

## class mch::ShaperMatch

- decl: `ton-index-worker/ton-mch-engine/src/HostRegistry.h:45`

Borrowed match context passed to a host shaper: named captures, the consumed set, and an arena for minting proxy blocks.

try_build stack-allocates one after merge, copies the match consumed set, maps each capture name to its first block or null when the capture is empty, and points arena at the BlockArena that minted produced before calling the registered shaper. The map and vector hold non-owning pointers; Block objects stay arena-owned and the struct dies when the shaper returns. Host shapers look up named blocks through capture, walk consumed for post-merge topology, and mint proxy blocks through arena.

## host_shapers

`const std::map<std::string, HostShaperFn> & host_shapers()`

- decl: `ton-index-worker/ton-mch-engine/src/HostRegistry.cpp:101`
- callers: mch::test_getgems_proxy_insert (ton-index-worker/ton-mch-engine/harness/ShapeTest.cpp:91); mch::test_getgems_proxy_insert (ton-index-worker/ton-mch-engine/harness/ShapeTest.cpp:92); mch::test_getgems_proxy_insert (ton-index-worker/ton-mch-engine/harness/ShapeTest.cpp:93); mch::run_surface (ton-index-worker/ton-mch-engine/harness/Surface.cpp:80); [mch::build_skip_reason (ton-index-worker/ton-mch-engine/src/BuildDriver.cpp:50)](BuildDriver.md#build_skip_reason); [mch::try_build (ton-index-worker/ton-mch-engine/src/ClassifyCore.cpp:160)](ClassifyCore.md#try_build); [mch::try_build (ton-index-worker/ton-mch-engine/src/ClassifyCore.cpp:161)](ClassifyCore.md#try_build)

Returns the registry of post-build shaper functions keyed by IR-visible name.

Four shapers — getgems_proxy_insert, nft_transfer_parent_absorb, nominator_withdraw_absorb_payouts and tgbtc_deploy_absorb — that reshape a matcher's produced block tree after merge, as the last step of try_build. Registration is also what keeps build_skip_reason from reporting a registered shaper as missing. Function-local static map, built once and immutable, safe for concurrent lookups.

## host_fns

`const std::map<std::string, HostFnEntry> & host_fns()`

- decl: `ton-index-worker/ton-mch-engine/src/HostRegistry.cpp:11`
- callers: mch::run_surface (ton-index-worker/ton-mch-engine/harness/Surface.cpp:76); [mch::build_skip_reason (ton-index-worker/ton-mch-engine/src/BuildDriver.cpp:44)](BuildDriver.md#build_skip_reason); mch::rt_call_hostfn (ton-index-worker/ton-mch-engine/src/HostRegistry.cpp:52); mch::rt_call_hostfn (ton-index-worker/ton-mch-engine/src/HostRegistry.cpp:53)

Returns the registry of host fn implementations keyed by IR-visible name.

Twenty-nine entries covering dedust, layerzero, tonstakers, vesting, nominator, nft, telegram, tgbtc, coffee, stonfi, tonco, evaa, getgems and cocoon helpers, each with a fixed arity except where -1 leaves the check to the implementation. The IR loader and build_skip_reason gate a matcher's inclusion on this registry, and generated build fns dispatch through rt_call_hostfn at classification time. Function-local static map: initialized on first use, immutable afterwards, so unsynchronized concurrent reads are fine.

## rt_call_hostfn

`EvalResult rt_call_hostfn(BuildEnv & env, const std::string & name, const std::vector<Value> & args)`

- decl: `ton-index-worker/ton-mch-engine/src/HostRegistry.cpp:50`
- callers: mch::build_54_stonfi_v2_swap (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:10062); mch::build_57_telegram_nft_purchase (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:10578); mch::build_58_telegram_nft_assigned (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:10681); mch::build_59_teleitem_start_auction (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:10844); mch::build_60_teleitem_cancel_auction (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:11030); mch::build_62_nft_put_on_sale (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:11525); mch::build_62_nft_put_on_sale (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:11533); mch::build_64_auction_bid (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:12040); mch::build_69_tgbtc_mint (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:12884); mch::build_70_tgbtc_burn (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:12949); mch::build_71_tgbtc_new_key (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:13061); mch::build_71_tgbtc_new_key (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:13065); mch::build_72_tgbtc_mint_log_only (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:13142); mch::build_73_tgbtc_burn_log_only (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:13207); mch::build_74_tgbtc_new_key_log_only (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:13251); mch::build_75_tgbtc_dkg_log_only (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:13303); mch::build_76_tonco_deposit_liquidity (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:13344); mch::build_7_coffee_swap (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:1351); mch::build_78_tonco_withdraw_liquidity (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:13611); mch::build_79_tonco_swap (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:13785); mch::build_80_tonstakers_delayed_withdrawal (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:13890); mch::build_83_vesting_send_message (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:14357); mch::build_94_cocoon_worker_payout (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:15349); mch::build_96_cocoon_proxy_charge (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:15563); mch::build_103_cocoon_grant_refund (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:16148); mch::build_9_dedust_deposit_final (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:1709); mch::build_10_dedust_deposit_partial (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:1823); mch::build_11_dedust_swap (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:1907); mch::build_15_dedust_v2_swap (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:3238); mch::build_15_dedust_v2_swap (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:3250); mch::build_15_dedust_v2_swap (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:3335); mch::build_15_dedust_v2_swap (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:3843); mch::build_16_dedust_v2_failed_swap (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:3910); mch::build_16_dedust_v2_failed_swap (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:3922); mch::build_16_dedust_v2_failed_swap (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:4380); mch::build_18_dedust_v2_deposit_final (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:4897); mch::build_18_dedust_v2_deposit_final (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:4909); mch::build_19_dedust_v2_deposit_rejected (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:5323); mch::build_19_dedust_v2_deposit_rejected (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:5335); mch::build_30_evaa_supply (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:6979); mch::build_32_evaa_liquidate (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:7390); mch::build_33_nft_transfer (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:7457); mch::build_39_layerzero_receive (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:8381); mch::build_50_nominator_pool_withdraw_request (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:9662); mch::build_51_stonfi_v1_swap (ton-index-worker/ton-mch-engine/generated/builds_ir_generated.cpp:9675)
- calls: `mch::EvalResult::EvalResult`, `mch::HostFnEntry::fn`, `mch::RejectCtx::RejectCtx`, `mch::RejectScope::RejectScope`, `mch::host_fns`, `mch::reject_ctx`, `mch::reject_log`, `mch::rt_fault`

Dispatches a host fn call for the build runtime, turning unknown names and arity mismatches into clean faults.

Looks the name up in host_fns and returns an unknown host fn fault on a miss. Before invoking, it installs a RejectScope carrying the fn name and the env's anchor so both the arity fault below and any host_reject inside the implementation annotate the mch-reject log line. A declared arity mismatching the argument count short-circuits with a bad arguments fault; otherwise the implementation runs and any faulted result is pushed to the reject log before the result is returned. Faults are match rejections — nothing here aborts or throws outward.

## host_predicates

`const std::map<std::string, HostPredFn> & host_predicates()`

- decl: `ton-index-worker/ton-mch-engine/src/HostRegistry.h:22`
- callers: mch::run_surface (ton-index-worker/ton-mch-engine/harness/Surface.cpp:79); [mch::match_skip_reason (ton-index-worker/ton-mch-engine/src/BuildDriver.cpp:15)](BuildDriver.md#match_skip_reason); [mch::MatchState::test_head (ton-index-worker/ton-mch-engine/src/Walker.cpp:154)](Walker.md#test_head); [mch::MatchState::test_head (ton-index-worker/ton-mch-engine/src/Walker.cpp:155)](Walker.md#test_head); [mch::MatchState::test_head (ton-index-worker/ton-mch-engine/src/Walker.cpp:166)](Walker.md#test_head); [mch::MatchState::test_head (ton-index-worker/ton-mch-engine/src/Walker.cpp:167)](Walker.md#test_head); [mch::MatchState::match_recursive (ton-index-worker/ton-mch-engine/src/Walker.cpp:246)](Walker.md#match_recursive); [mch::MatchState::match_recursive (ton-index-worker/ton-mch-engine/src/Walker.cpp:247)](Walker.md#match_recursive); [mch::matcher_test_self (ton-index-worker/ton-mch-engine/src/Walker.cpp:404)](Walker.md#matcher_test_self); [mch::matcher_test_self (ton-index-worker/ton-mch-engine/src/Walker.cpp:405)](Walker.md#matcher_test_self); [mch::matcher_test_self (ton-index-worker/ton-mch-engine/src/Walker.cpp:421)](Walker.md#matcher_test_self); [mch::matcher_test_self (ton-index-worker/ton-mch-engine/src/Walker.cpp:422)](Walker.md#matcher_test_self)

Returns the registry of block predicates keyed by IR-visible name.

Seventeen entries, mostly evaa and NFT-sale predicates plus the anchor-style single_contract_deploy, sale_contract_deploy and pton_self_transfer. A matcher is runnable iff all its predicate names are here, with missing names becoming its skip reason; Walker's match state calls them per block during matching. Predicate anchors get no opcode or block-type prefilter. Function-local static map, built once and immutable, safe for concurrent reads.

## fn

`fn`

- decl: `ton-index-worker/ton-mch-engine/src/HostRegistry.h:32`
- callers: mch::rt_call_hostfn (ton-index-worker/ton-mch-engine/src/HostRegistry.cpp:68)

The host fn callable that rt_call_hostfn invokes after arity validation.

The signature receives the BuildEnv so an implementation can reach env.lookups and env.consumed; arguments pass through unchanged and a null argument is not short-circuited. A thrown or faulted result is a clean match rejection, and returning Null likewise rejects when the spec rejects on null.

## ShaperMatch

`void ShaperMatch()`

- decl: `ton-index-worker/ton-mch-engine/src/HostRegistry.h:45`
- callers: mch::test_getgems_proxy_insert (ton-index-worker/ton-mch-engine/harness/ShapeTest.cpp:97); [mch::try_build (ton-index-worker/ton-mch-engine/src/ClassifyCore.cpp:162)](ClassifyCore.md#try_build)

Implicitly default-constructed; empty captures and consumed, null arena.


## capture

`Block * capture(const std::string & name) const`

- decl: `ton-index-worker/ton-mch-engine/src/HostRegistry.h:50`
- callers: [mch::getgems_proxy_insert (ton-index-worker/ton-mch-engine/src/host/HostNftSale.cpp:499)](HostNftSale.md#getgems_proxy_insert); [mch::nominator_withdraw_absorb_payouts (ton-index-worker/ton-mch-engine/src/host/HostNominator.cpp:61)](HostNominator.md#nominator_withdraw_absorb_payouts)

Looks up a captured block by name, nullptr when the capture is absent.

A plain find over the captures map, which try_build fills from the match result's captures (first value per name, which may itself be a null block pointer for an empty capture). Returning nullptr for absent names lets a shaper treat the capture as optional topology rather than an error. Infallible.
