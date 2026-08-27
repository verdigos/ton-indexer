---
module: DexPton
source: ton-index-worker/ton-mch-engine/src/host/DexPton.cpp
source-commit: 06d4c50
---

# DexPton

pTON handling shared by the Stonfi and Tonco bindings: a fixed allowlist of the three canonical pTON jetton master addresses and a wallet-to-asset conversion selected by a flag. With conversion on, a wallet whose jetton master is on the allowlist becomes a native TON asset and an unresolvable master becomes Null; with conversion off, pTON stays a jetton asset and an unresolvable master becomes TON. Master strings are canonicalized through the shared normalize-or-passthrough policy before becoming jetton assets, while the allowlist check itself compares the raw field text exactly.

## is_pton_master

`bool is_pton_master(const std::string & s)`

- decl: `ton-index-worker/ton-mch-engine/src/host/DexPton.cpp:15`
- callers: mch::test_pton_helper (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:191); mch::test_pton_helper (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:192); mch::wallet_jetton_asset (ton-index-worker/ton-mch-engine/src/host/DexPton.cpp:38); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:691)](HostStonfi.md#stonfi_v2_swap_data); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:805)](HostStonfi.md#stonfi_v2_swap_data); [mch::tonco_leg_asset (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:512)](HostTonco.md#tonco_leg_asset)
- calls: `mch::pton_masters`

Exact string match against the three canonical pTON master addresses.

The comparison is exact-spelling: canonicalization applies only to the Asset string built afterwards, so a non-canonical rendering of a pTON master is not converted and falls through to a jetton asset.

## wallet_jetton_master_str

`std::optional<std::string> wallet_jetton_master_str(const Value & wallet)`

- decl: `ton-index-worker/ton-mch-engine/src/host/DexPton.cpp:20`
- callers: mch::wallet_jetton_asset (ton-index-worker/ton-mch-engine/src/host/DexPton.cpp:30); [mch::dedust_deposit_final_data (ton-index-worker/ton-mch-engine/src/host/HostDedustDeposit.cpp:260)](HostDedustDeposit.md#dedust_deposit_final_data); [mch::dedust_deposit_final_data (ton-index-worker/ton-mch-engine/src/host/HostDedustDeposit.cpp:289)](HostDedustDeposit.md#dedust_deposit_final_data); [mch::dedust_deposit_partial_data (ton-index-worker/ton-mch-engine/src/host/HostDedustDeposit.cpp:493)](HostDedustDeposit.md#dedust_deposit_partial_data); [mch::stonfi_v1_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:449)](HostStonfi.md#stonfi_v1_swap_data); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:853)](HostStonfi.md#stonfi_v2_swap_data)
- calls: `mch::Value::field`, `mch::Value::is_null`, `mch::canonicalize_or_passthrough`

The wallet's jetton master string, canonicalized; nullopt on a null wallet, a missing jetton field, or a non-string jetton.

Canonicalization is normalize-or-passthrough, so text that fails normalization is returned unchanged. Conversion to an asset stays with the caller.

## wallet_jetton_asset

`Value wallet_jetton_asset(const Value & wallet, bool pton_conversion)`

- decl: `ton-index-worker/ton-mch-engine/src/host/DexPton.cpp:28`
- callers: mch::test_pton_helper (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:195); mch::test_pton_helper (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:196); mch::test_pton_helper (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:200); mch::test_pton_helper (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:202); mch::test_pton_helper (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:206); [mch::stonfi_v1_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:421)](HostStonfi.md#stonfi_v1_swap_data); [mch::stonfi_v1_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:429)](HostStonfi.md#stonfi_v1_swap_data); [mch::asset_from_wallet_jetton (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:544)](HostStonfi.md#asset_from_wallet_jetton); [mch::wallet_asset (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:47)](HostTonco.md#wallet_asset)
- calls: `mch::Value::field`, `mch::Value::is_null`, `mch::Value::make_asset_jetton`, `mch::Value::make_asset_ton`, `mch::Value::null`, `mch::is_pton_master`, `mch::wallet_jetton_master_str`

Turns a jetton-wallet lookup Value into an asset, applying pTON conversion when the flag is set.

When no master resolves, conversion on (Stonfi v2, Tonco) yields Null while conversion off (Stonfi v1) yields a TON asset. With conversion on, a jetton field that exactly matches a pTON master yields a TON asset; anything else yields a jetton asset carrying the canonicalized master. A null wallet counts as a lookup miss and takes the no-master branch.

## pton_masters

`const std::vector<std::string> & pton_masters()`

- decl: `ton-index-worker/ton-mch-engine/src/host/DexPton.cpp:7`
- callers: mch::test_pton_helper (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:188); mch::is_pton_master (ton-index-worker/ton-mch-engine/src/host/DexPton.cpp:16)

The process-wide allowlist of three canonical pTON master addresses in raw workchain-colon-HEX upper form.

A function-local static returned by reference, initialized once. Stonfi v2 and Tonco both treat a jetton wallet on one of these masters as native TON.
