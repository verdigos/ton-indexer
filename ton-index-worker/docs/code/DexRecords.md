---
module: DexRecords
source: ton-index-worker/ton-mch-engine/devtools/../src/host/DexRecords.h
source: ton-index-worker/ton-mch-engine/src/host/DexRecords.cpp
source: ton-index-worker/ton-mch-engine/src/host/DexRecords.h
source-commit: 06d4c50
---

# DexRecords

Typed record vocabulary shared by the DEX swap bindings: SwapLeg, TransferLeg, PeerSwap and SwapRecord, each with one encoder into the engine's Value world, plus a reader that lifts a produced jetton-transfer block's data dict into a TransferLeg. One encoder owns the output field names and null conventions so Coffee and Stonfi emit identical record shapes; peer-swap out-assets are back-filled from the following hop before encoding rather than by each core. The records are plain structs of Values assembled by a protocol core and encoded once at the end; nothing here owns blocks or carries lifetime of its own.

## class mch::SwapLeg

- decl: `ton-index-worker/ton-mch-engine/src/host/DexRecords.h:20`

One side of a peer-swap hop: an asset and an amount.


## class mch::TransferLeg

- decl: `ton-index-worker/ton-mch-engine/src/host/DexRecords.h:28`

A full transfer leg: asset, amount, source and destination accounts, and the two jetton wallets.

Six Value fields mirroring the jetton-transfer vocabulary; built field by field by a core or wholesale via from_jetton_transfer, then moved into a SwapRecord's incoming and outgoing legs. encode renders the six fields in one fixed order shared by every DEX, so cores never format legs themselves.

## class mch::PeerSwap

- decl: `ton-index-worker/ton-mch-engine/src/host/DexRecords.h:44`

One peer-swap hop: an in leg and an out leg.

Cores fill both legs' amounts and the in assets; out assets are deliberately left for SwapRecord's fill_peer_out_assets, which chains them from the following hop. Encodes as a Dict of the encoded in and out legs.

## class mch::SwapRecord

- decl: `ton-index-worker/ton-mch-engine/src/host/DexRecords.h:51`

The top-level swap record: dex name, sender, source and destination assets, both DEX transfer legs, peer swaps, referral fields and a failed flag.

Assembled by a protocol core, completed with fill_peer_out_assets and rendered by encode into the Obj handed back to the engine. The field set and order mirror coffee_swap_data, which is the canonical shape. failed defaults to false, and peer_swaps reaches the output only for multi-hop swaps.

## SwapLeg

`void SwapLeg()`

- decl: `ton-index-worker/ton-mch-engine/devtools/../src/host/DexRecords.h:20`
- callers: mch::test_swap_leg_shape (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:60)

Default constructor; asset and amount start as null Values.


## SwapRecord

`void SwapRecord(SwapRecord &&)`

- decl: `ton-index-worker/ton-mch-engine/devtools/../src/host/DexRecords.h:51`
- callers: mch::sample_record (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:94)

Move constructor; an assembled record is returned by value.


## from_jetton_transfer

`TransferLeg from_jetton_transfer(const Block * jt)`

- decl: `ton-index-worker/ton-mch-engine/src/host/DexRecords.cpp:13`
- callers: [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:132)](HostCoffee.md#coffee_swap); [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:161)](HostCoffee.md#coffee_swap); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:694)](HostStonfi.md#stonfi_v2_swap_data); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:806)](HostStonfi.md#stonfi_v2_swap_data)
- calls: `mch::TransferLeg::TransferLeg`, `mch::data_field`

Lifts a produced jetton-transfer block's data dict into the six TransferLeg fields.

Reads asset, amount, sender, sender_wallet, receiver and receiver_wallet into asset, amount, source, source_jetton_wallet, destination and destination_jetton_wallet; any absent key comes back Null from data_field rather than erroring. This is the shared leg shape reused by Coffee's in and out arms and by the Stonfi cores.

## encode

`Value encode() const`

- decl: `ton-index-worker/ton-mch-engine/src/host/DexRecords.cpp:24`
- callers: mch::test_swap_leg_shape (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:63); mch::PeerSwap::encode (ton-index-worker/ton-mch-engine/src/host/DexRecords.cpp:44); mch::PeerSwap::encode (ton-index-worker/ton-mch-engine/src/host/DexRecords.cpp:45)
- calls: `mch::Value::make_dict`

Dict with asset then amount.


## encode

`Value encode() const`

- decl: `ton-index-worker/ton-mch-engine/src/host/DexRecords.cpp:31`
- callers: mch::test_transfer_leg_shape (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:49); mch::SwapRecord::encode (ton-index-worker/ton-mch-engine/src/host/DexRecords.cpp:71); mch::SwapRecord::encode (ton-index-worker/ton-mch-engine/src/host/DexRecords.cpp:72); [mch::stonfi_v1_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:458)](HostStonfi.md#stonfi_v1_swap_data); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:696)](HostStonfi.md#stonfi_v2_swap_data); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:808)](HostStonfi.md#stonfi_v2_swap_data); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:833)](HostStonfi.md#stonfi_v2_swap_data)
- calls: `mch::Value::make_dict`

Dict of the six fields in declaration order.


## encode

`Value encode() const`

- decl: `ton-index-worker/ton-mch-engine/src/host/DexRecords.cpp:42`
- callers: mch::test_peer_swap_shape (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:75); mch::SwapRecord::encode (ton-index-worker/ton-mch-engine/src/host/DexRecords.cpp:63)
- calls: `mch::SwapLeg::encode`, `mch::Value::make_dict`

Dict with the encoded in and out legs.


## fill_peer_out_assets

`void fill_peer_out_assets()`

- decl: `ton-index-worker/ton-mch-engine/src/host/DexRecords.cpp:49`
- callers: mch::test_fill_peer_out_assets (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:144); [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:205)](HostCoffee.md#coffee_swap)

Chains each hop's out asset from the next hop's in asset; the last hop takes the DEX outgoing transfer's asset.

No-op when there are no peer swaps. Cores call it after assembling the hops and before encode, so multi-hop records never carry an unset out asset.

## encode

`Value encode() const`

- decl: `ton-index-worker/ton-mch-engine/src/host/DexRecords.cpp:59`
- callers: mch::test_record_peer_gate (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:112); mch::test_record_peer_gate (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:123); mch::test_record_peer_gate (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:129); mch::test_record_shape (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:99); [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:206)](HostCoffee.md#coffee_swap)
- calls: `mch::PeerSwap::encode`, `mch::TransferLeg::encode`, `mch::Value::make_bool`, `mch::Value::make_list`, `mch::Value::make_obj`, `mch::Value::make_str`

Renders the final Obj with dex, sender, source_asset, destination_asset, both transfer legs, peer_swaps, referral fields and failed.

peer_swaps encodes as a list of hop Dicts only when more than one hop exists, otherwise an empty list, matching Coffee's convention. failed becomes a Bool, and the referral fields pass through as the stored Values.

## excess_pair

`Value excess_pair(Value asset, Value amount)`

- decl: `ton-index-worker/ton-mch-engine/src/host/DexRecords.cpp:9`
- callers: [mch::dedust_deposit_final_data (ton-index-worker/ton-mch-engine/src/host/HostDedustDeposit.cpp:310)](HostDedustDeposit.md#dedust_deposit_final_data); [mch::dedust_deposit_final_data (ton-index-worker/ton-mch-engine/src/host/HostDedustDeposit.cpp:317)](HostDedustDeposit.md#dedust_deposit_final_data); [mch::tonco_deposit_liquidity_data (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:365)](HostTonco.md#tonco_deposit_liquidity_data)
- calls: `mch::Value::make_list`

The shared two-element asset, amount list used for excess refunds.


## TransferLeg

`void TransferLeg()`

- decl: `ton-index-worker/ton-mch-engine/src/host/DexRecords.h:28`
- callers: mch::test_transfer_leg_shape (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:42); mch::TransferLeg::from_jetton_transfer (ton-index-worker/ton-mch-engine/src/host/DexRecords.cpp:14); [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:129)](HostCoffee.md#coffee_swap); [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:159)](HostCoffee.md#coffee_swap); [mch::ton_leg (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:40)](HostCoffee.md#ton_leg); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:826)](HostStonfi.md#stonfi_v2_swap_data)

Default constructor; the six Values start null.


## TransferLeg

`void TransferLeg(TransferLeg &&)`

- decl: `ton-index-worker/ton-mch-engine/src/host/DexRecords.h:28`
- callers: mch::TransferLeg::from_jetton_transfer (ton-index-worker/ton-mch-engine/src/host/DexRecords.cpp:21); [mch::ton_leg (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:47)](HostCoffee.md#ton_leg)

Move constructor.


## operator=

`TransferLeg & operator=(TransferLeg &&)`

- decl: `ton-index-worker/ton-mch-engine/src/host/DexRecords.h:28`
- callers: [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:132)](HostCoffee.md#coffee_swap); [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:154)](HostCoffee.md#coffee_swap); [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:161)](HostCoffee.md#coffee_swap); [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:183)](HostCoffee.md#coffee_swap); [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:189)](HostCoffee.md#coffee_swap); [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:198)](HostCoffee.md#coffee_swap); [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:199)](HostCoffee.md#coffee_swap)

Move assignment used by cores to replace a leg's contents during assembly.


## PeerSwap

`void PeerSwap()`

- decl: `ton-index-worker/ton-mch-engine/src/host/DexRecords.h:44`
- callers: mch::test_record_peer_gate (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:117); mch::test_fill_peer_out_assets (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:138); mch::test_peer_swap_shape (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:70); [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:115)](HostCoffee.md#coffee_swap)

Default constructor; in and out start as null legs.


## SwapRecord

`void SwapRecord()`

- decl: `ton-index-worker/ton-mch-engine/src/host/DexRecords.h:51`
- callers: mch::sample_record (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:84); [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:193)](HostCoffee.md#coffee_swap)

Default constructor; failed starts false.

