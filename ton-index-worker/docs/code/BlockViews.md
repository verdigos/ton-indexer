---
module: BlockViews
source: ton-index-worker/ton-mch-engine/src/host/BlockViews.cpp
source-commit: 06d4c50
---

# BlockViews

Query helpers over consumed block sets and the block graph, used by the Coffee, Dedust, Stonfi, Tonco, TGBTC and NFT-sale bindings to pick message blocks out of a match. The scans match on call-contract btype plus exact opcode; unique_lt_sorted deduplicates by pointer and stable-sorts by minimum logical time so multi-hop successor lists come out canonical. Truthiness and account-string helpers fix the shared notions of a present data field and a usable address. Everything operates on borrowed pointers and mutates only caller-owned vectors.

## first_call_impl

`const Block * first_call_impl()`

- decl: `ton-index-worker/ton-mch-engine/src/host/BlockViews.cpp:15`
- callers: none found in parsed TU set
- calls: `mch::is_call_op`

Template behind both first_call overloads, instantiated per pointer constness.


## first_call_impl

`const Block * first_call_impl(const std::vector<const Block *> & blocks, std::uint32_t op)`

- decl: `ton-index-worker/ton-mch-engine/src/host/BlockViews.cpp:15`
- callers: mch::first_call (ton-index-worker/ton-mch-engine/src/host/BlockViews.cpp:31)

Scans blocks in order and returns the first one matching the call opcode, or nullptr.


## first_call_impl

`const Block * first_call_impl(const std::vector<Block *> & blocks, std::uint32_t op)`

- decl: `ton-index-worker/ton-mch-engine/src/host/BlockViews.cpp:15`
- callers: mch::first_call (ton-index-worker/ton-mch-engine/src/host/BlockViews.cpp:35)

The same scan instantiated for vectors of non-const Block pointers.


## block_in

`bool block_in(const std::vector<const Block *> & v, const Block * b)`

- decl: `ton-index-worker/ton-mch-engine/src/host/BlockViews.cpp:26`
- callers: [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:60)](HostCoffee.md#coffee_swap)

Linear membership test comparing Block pointer identity.


## first_call

`const Block * first_call(const std::vector<const Block *> & blocks, std::uint32_t op)`

- decl: `ton-index-worker/ton-mch-engine/src/host/BlockViews.cpp:30`
- callers: [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:65)](HostCoffee.md#coffee_swap); [mch::dedust_deposit_partial_data (ton-index-worker/ton-mch-engine/src/host/HostDedustDeposit.cpp:359)](HostDedustDeposit.md#dedust_deposit_partial_data); [mch::tgbtc_mint_data (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:145)](HostTgbtc.md#tgbtc_mint_data); [mch::tgbtc_mint_data (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:146)](HostTgbtc.md#tgbtc_mint_data)
- calls: `mch::first_call_impl`

Returns the first block in the vector with the given call opcode, nullptr when none matches.


## first_call

`const Block * first_call(const std::vector<Block *> & blocks, std::uint32_t op)`

- decl: `ton-index-worker/ton-mch-engine/src/host/BlockViews.cpp:34`
- callers: [mch::resolve_leg (ton-index-worker/ton-mch-engine/src/host/HostDedustDeposit.cpp:74)](HostDedustDeposit.md#resolve_leg); [mch::auction_bid_data (ton-index-worker/ton-mch-engine/src/host/HostNftSale.cpp:479)](HostNftSale.md#auction_bid_data); [mch::tonco_deposit_liquidity_data (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:254)](HostTonco.md#tonco_deposit_liquidity_data); [mch::tonco_deposit_liquidity_data (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:354)](HostTonco.md#tonco_deposit_liquidity_data); [mch::tonco_withdraw_payouts (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:456)](HostTonco.md#tonco_withdraw_payouts)
- calls: `mch::first_call_impl`

First-match scan over non-const Block pointers, forwarding to the shared template.


## all_calls

`std::vector<const Block *> all_calls(const std::vector<const Block *> & blocks, std::uint32_t op)`

- decl: `ton-index-worker/ton-mch-engine/src/host/BlockViews.cpp:38`
- callers: [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:85)](HostCoffee.md#coffee_swap)
- calls: `mch::is_call_op`

Collects every block in the vector with the given call opcode, preserving input order.


## first_next_call

`const Block * first_next_call(const Block * b, std::uint32_t op)`

- decl: `ton-index-worker/ton-mch-engine/src/host/BlockViews.cpp:48`
- callers: [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:92)](HostCoffee.md#coffee_swap)
- calls: `mch::is_call_op`

Returns the first successor of b with the given call opcode; nullptr for a null b or no matching successor.


## unique_lt_sorted

`void unique_lt_sorted(std::vector<const Block *> & blocks)`

- decl: `ton-index-worker/ton-mch-engine/src/host/BlockViews.cpp:60`
- callers: [mch::coffee_swap (ton-index-worker/ton-mch-engine/src/host/HostCoffee.cpp:87)](HostCoffee.md#coffee_swap)

Deduplicates by pointer keeping first occurrence, then stable-sorts ascending by minimum logical time.

Stability means ties on min_lt keep first-occurrence order, so the output is deterministic for a given input sequence. coffee_swap runs it over gathered successor lists before reading hops.

## value_truthy

`bool value_truthy(const Value & v)`

- decl: `ton-index-worker/ton-mch-engine/src/host/BlockViews.cpp:72`
- callers: mch::data_truthy (ton-index-worker/ton-mch-engine/src/host/BlockViews.cpp:78); [mch::dedust_seq_destination (ton-index-worker/ton-mch-engine/src/host/HostDedust.cpp:54)](HostDedust.md#dedust_seq_destination)

True for a true Bool or a non-null non-zero Int; false for every other Value, including Null.


## data_truthy

`bool data_truthy(const Block * b, const char * field)`

- decl: `ton-index-worker/ton-mch-engine/src/host/BlockViews.cpp:77`
- callers: mch::test_data_truthy (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:219); mch::test_data_truthy (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:220); mch::test_data_truthy (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:221); mch::test_data_truthy (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:222); mch::test_data_truthy (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:223); mch::test_data_truthy (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:224); [mch::stonfi_v1_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:409)](HostStonfi.md#stonfi_v1_swap_data); [mch::stonfi_v1_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:412)](HostStonfi.md#stonfi_v1_swap_data); [mch::pton_self_transfer (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:480)](HostStonfi.md#pton_self_transfer); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:795)](HostStonfi.md#stonfi_v2_swap_data)
- calls: `mch::data_field`, `mch::value_truthy`

Truthiness of a named field in the block's data dict, where a missing field reads as Null and is false.


## acc_str

`std::optional<std::string> acc_str(const Value & v)`

- decl: `ton-index-worker/ton-mch-engine/src/host/BlockViews.cpp:81`
- callers: mch::test_acc_str (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:229); mch::test_acc_str (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:231); mch::test_acc_str (ton-index-worker/ton-mch-engine/harness/DexRecordsTest.cpp:232); [mch::pool_accounts (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:191)](HostStonfi.md#pool_accounts); [mch::pool_accounts (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:205)](HostStonfi.md#pool_accounts); [mch::extract_target_wallet (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:273)](HostStonfi.md#extract_target_wallet); [mch::stonfi_v1_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:385)](HostStonfi.md#stonfi_v1_swap_data); [mch::stonfi_v1_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:386)](HostStonfi.md#stonfi_v1_swap_data); [mch::stonfi_v1_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:387)](HostStonfi.md#stonfi_v1_swap_data); [mch::pton_self_transfer (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:491)](HostStonfi.md#pton_self_transfer); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:662)](HostStonfi.md#stonfi_v2_swap_data); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:689)](HostStonfi.md#stonfi_v2_swap_data); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:732)](HostStonfi.md#stonfi_v2_swap_data); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:746)](HostStonfi.md#stonfi_v2_swap_data); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:764)](HostStonfi.md#stonfi_v2_swap_data); [mch::stonfi_v2_swap_data (ton-index-worker/ton-mch-engine/src/host/HostStonfi.cpp:803)](HostStonfi.md#stonfi_v2_swap_data); [mch::collect_payload_targets (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:121)](HostTonco.md#collect_payload_targets); [mch::tonco_withdraw_payouts (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:431)](HostTonco.md#tonco_withdraw_payouts); [mch::tonco_withdraw_payouts (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:437)](HostTonco.md#tonco_withdraw_payouts); [mch::tonco_withdraw_payouts (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:476)](HostTonco.md#tonco_withdraw_payouts); [mch::tonco_withdraw_payouts (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:487)](HostTonco.md#tonco_withdraw_payouts); [mch::tonco_withdraw_payouts (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:488)](HostTonco.md#tonco_withdraw_payouts); [mch::tonco_swap_data (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:629)](HostTonco.md#tonco_swap_data); [mch::tonco_swap_data (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:695)](HostTonco.md#tonco_swap_data); [mch::tonco_swap_data (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:696)](HostTonco.md#tonco_swap_data); [mch::tonco_swap_data (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:720)](HostTonco.md#tonco_swap_data); [mch::tonco_swap_data (ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp:731)](HostTonco.md#tonco_swap_data)

The canonical address string of an Account Value that is not addr_none; nullopt otherwise.

