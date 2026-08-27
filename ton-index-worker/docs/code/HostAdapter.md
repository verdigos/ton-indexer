---
module: HostAdapter
source: ton-index-worker/ton-mch-engine/src/host/HostAdapter.cpp
source: ton-index-worker/ton-mch-engine/src/host/HostAdapter.h
source-commit: 06d4c50
---

# HostAdapter

Consumed-set decode helpers shared by the swap-style host bindings registered in the engine's host-fn table. The registry validates arity and faults on unknown names before dispatch, so these helpers only turn the single List argument of a binding such as coffee_swap_data or stonfi_v2_swap_data into borrowed Block pointers, dropping any non-Block elements. A decode succeeds only when the list is non-empty and contains at least one Block, so a successfully decoded ConsumedBlocks always has an anchor. Everything is a stateless free function over pointers owned by the block tree; there is no ownership transfer or shared mutable state.

## class mch::ConsumedBlocks

- decl: `ton-index-worker/ton-mch-engine/src/host/HostAdapter.h:18`

The decoded consumed match set: a vector of borrowed Block pointers whose element 0 is the anchor and whose tail is the rest of the match.

Plain value struct; blocks holds non-owning pointers into the block tree, and both decode paths guarantee the vector is non-empty on success. Swap-style host bindings build it from the single List argument and then read the anchor and the remaining blocks through the accessors. No cleanup or lifetime management is involved.

## decode_consumed_or_none

`std::optional<ConsumedBlocks> decode_consumed_or_none(const std::vector<Value> & args)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostAdapter.cpp:26`
- callers: [mch::dedust_deposit_final_data (ton-index-worker/ton-mch-engine/src/host/HostDedustDeposit.cpp:123)](HostDedustDeposit.md#dedust_deposit_final_data); [mch::dedust_deposit_partial_data (ton-index-worker/ton-mch-engine/src/host/HostDedustDeposit.cpp:343)](HostDedustDeposit.md#dedust_deposit_partial_data); [mch::evaa_supply_data (ton-index-worker/ton-mch-engine/src/host/HostEvaa.cpp:271)](HostEvaa.md#evaa_supply_data); [mch::evaa_liquidate_data (ton-index-worker/ton-mch-engine/src/host/HostEvaa.cpp:405)](HostEvaa.md#evaa_liquidate_data); [mch::tgbtc_mint_data (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:140)](HostTgbtc.md#tgbtc_mint_data); [mch::tonco_deposit_liquidity_data (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:151)](HostTonco.md#tonco_deposit_liquidity_data); [mch::tonco_withdraw_payouts (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:397)](HostTonco.md#tonco_withdraw_payouts); [mch::tonco_swap_data (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:583)](HostTonco.md#tonco_swap_data)
- calls: `mch::ConsumedBlocks::ConsumedBlocks`, `mch::as_block`

Decodes the consumed-set argument, returning nullopt instead of faulting when the argument is not a non-empty List or yields no Block.

Same decode as the faulting variant, for hosts that reject rather than fault: nullopt when the first argument is not a non-empty List or when no element is a Block. Non-Block elements are silently dropped from a successful decode. Arity has already been checked by the registry, so only the List shape is validated here.

## decode_consumed

`EvalResult decode_consumed(const std::vector<Value> & args, const char * binding, ConsumedBlocks & out)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostAdapter.cpp:8`
- callers: mch::test_decode_consumed (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:170); mch::test_decode_consumed (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:173); mch::test_decode_consumed (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:176); mch::test_decode_consumed (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:178); [mch::coffee_swap_data (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:215)](HostCoffee.md#coffee_swap_data); [mch::dedust_swap_legs (ton-index-worker/ton-mch-engine/src/host/HostDedust.cpp:110)](HostDedust.md#dedust_swap_legs); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:554)](HostStonfi.md#stonfi_v2_swap_data)
- calls: `mch::Value::null`, `mch::as_block`, `mch::rt_fault`, `mch::rt_ok`

Decodes the single List argument into out, faulting with the binding name on a bad argument or an empty Block set.

Requires the first argument to be a non-empty List and otherwise faults with the binding name plus bad arguments. Clears out, appends the Block pointer of every Block-typed element while silently dropping the rest, and faults with the binding name plus empty consumed when nothing survived. On success returns an ok result carrying a null Value. Arity was already validated by the registry before dispatch.

## ConsumedBlocks

`void ConsumedBlocks()`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostAdapter.h:18`
- callers: mch::test_decode_consumed (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:166); mch::decode_consumed_or_none (ton-index-worker/ton-mch-engine/src/host/HostAdapter.cpp:30); [mch::coffee_swap_data (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:214)](HostCoffee.md#coffee_swap_data); [mch::dedust_swap_legs (ton-index-worker/ton-mch-engine/src/host/HostDedust.cpp:109)](HostDedust.md#dedust_swap_legs); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:553)](HostStonfi.md#stonfi_v2_swap_data)

Default constructor; leaves blocks empty for the caller to fill.


## anchor

`const Block * anchor() const`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostAdapter.h:21`
- callers: mch::test_decode_consumed (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:179); [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:54)](HostCoffee.md#coffee_swap)

Returns element 0 of the consumed set, the anchor block of the match.

Undefined behavior on an empty set; both decode paths fault or return nullopt before an empty set can reach a caller.

## others

`std::vector<const Block *> others() const`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostAdapter.h:22`
- callers: mch::test_decode_consumed (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:180); [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:55)](HostCoffee.md#coffee_swap)

Returns a copy of the consumed set without its anchor; an empty set yields an empty vector.

