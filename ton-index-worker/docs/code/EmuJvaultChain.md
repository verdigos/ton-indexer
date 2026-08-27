---
module: EmuJvaultChain
source: ton-index-worker/ton-mch-engine/src/emu/EmuJvaultChain.cpp
source: ton-index-worker/ton-mch-engine/src/emu/EmuJvaultChain.h
source-commit: 06d4c50
---

# EmuJvaultChain

Raw data-cell parsers and value builders for the JVault asset chain: the stake wallet names a staking pool, the pool names a lock wallet, and the lock wallet leads to a jetton master. Account reads and the jetton wallet step stay with the caller, which owns the lookup memo and budget. All parsers are pure functions returning td::Result with jvault-prefixed error messages, treating null, short, pruned, and exotic cells as parse failures, and the record builders assemble fixed-shape Value objects in which a null asset is the legitimate partial case.

## class mch::JvaultStakeWallet

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuJvaultChain.h:14`

Parsed stake-wallet result: pool address value, canonical raw pool string, optional minter.

staking_pool is an Account value that is never addr_none, staking_pool_raw is the canonical wc:HEX form used as the next lookup key, and minter is nullopt when the field is absent or not an address. A missing pool never produces this struct — it fails the parse outright.

## data_slice

`td::Result<vm::CellSlice> data_slice(const td::Ref<vm::Cell> & data)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuJvaultChain.cpp:12`
- callers: mch::parse_jvault_stake_wallet (ton-index-worker/ton-mch-engine/src/emu/EmuJvaultChain.cpp:34); mch::parse_jvault_pool_lock_wallet (ton-index-worker/ton-mch-engine/src/emu/EmuJvaultChain.cpp:50)

Loads an account data cell as a CellSlice or fails.

A null cell is an immediate error, and a throw from load_cell_slice_special is caught and converted, so short, pruned, and exotic cells all become td::Status errors rather than escaping exceptions.

## account_canonical

`std::optional<std::string> account_canonical(const Value & v)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuJvaultChain.cpp:24`
- callers: mch::parse_jvault_stake_wallet (ton-index-worker/ton-mch-engine/src/emu/EmuJvaultChain.cpp:36); mch::parse_jvault_stake_wallet (ton-index-worker/ton-mch-engine/src/emu/EmuJvaultChain.cpp:45); mch::parse_jvault_pool_lock_wallet (ton-index-worker/ton-mch-engine/src/emu/EmuJvaultChain.cpp:59)
- calls: `mch::canonicalize_or_passthrough`

Canonicalizes an Account value to raw form; nullopt when it is not an address.

Values that are not Account type or that carry addr_none yield nullopt; otherwise the string passes through the shared canonicalize-or-passthrough policy, which returns the input unchanged when normalization fails.

## parse_jvault_stake_wallet

`td::Result<JvaultStakeWallet> parse_jvault_stake_wallet(const td::Ref<vm::Cell> & data)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuJvaultChain.h:19`
- callers: mch::test_jvault_parsers (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:231); mch::test_jvault_parsers (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:243); mch::test_jvault_parsers (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:247); mch::test_jvault_parsers (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:256); mch::test_jvault_parsers (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:258); [mch::EmuCelldbTier2::jvault_assets (ton-index-worker/ton-mch-engine/src/emu/EmuCelldbLookup.cpp:133)](EmuCelldbLookup.md#jvault_assets)
- calls: `mch::account_canonical`, `mch::data_slice`, `mch::load_address_py`

Parses a stake-wallet data cell into pool and optional minter addresses.

Loads the data slice, then reads the staking pool and minter addresses with the TL-B address loader. A pool that is missing or not an address is an error that invalidates the complete record, while the minter may be absent; the pool comes back both as an Account value and in canonical raw form for the next lookup.

## parse_jvault_pool_lock_wallet

`td::Result<std::string> parse_jvault_pool_lock_wallet(const td::Ref<vm::Cell> & data)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuJvaultChain.h:23`
- callers: mch::test_jvault_parsers (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:261); mch::test_jvault_parsers (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:271); mch::test_jvault_parsers (ton-index-worker/ton-mch-engine/harness/EmuCelldbLookupTest.cpp:278); [mch::EmuCelldbTier2::jvault_assets (ton-index-worker/ton-mch-engine/src/emu/EmuCelldbLookup.cpp:150)](EmuCelldbLookup.md#jvault_assets)
- calls: `mch::account_canonical`, `mch::data_slice`, `mch::load_address_py`

Extracts the canonical lock-wallet address from a pool data cell.

Advances past a 33-bit header plus the admin and creator addresses, which are parsed for structure but discarded, then reads the lock wallet. Slice underflow and a missing or non-address lock wallet are errors; on success only the canonical raw string is returned.

## jvault_record

`Value jvault_record(const Value & staking_pool, Value asset, Value jvault_asset)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuJvaultChain.h:27`
- callers: [mch::EmuCelldbTier2::jvault_assets (ton-index-worker/ton-mch-engine/src/emu/EmuCelldbLookup.cpp:147)](EmuCelldbLookup.md#jvault_assets); [mch::EmuCelldbTier2::jvault_assets (ton-index-worker/ton-mch-engine/src/emu/EmuCelldbLookup.cpp:160)](EmuCelldbLookup.md#jvault_assets)
- calls: `mch::Value::make_obj`

Builds the three-key object holding staking_pool, asset, and jvault_asset.

Assembles the fields in fixed order into a Value object. A null asset is the legitimate partial case — the pool resolved but its state was absent — while jvault_asset carries the chain's own asset.

## jvault_asset_of

`Value jvault_asset_of(const std::optional<std::string> & canonical_master)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuJvaultChain.h:30`
- callers: [mch::EmuCelldbTier2::jvault_assets (ton-index-worker/ton-mch-engine/src/emu/EmuCelldbLookup.cpp:138)](EmuCelldbLookup.md#jvault_assets)
- calls: `mch::Value::make_asset_jetton`, `mch::Value::null`

Wraps a canonical master address as a jetton asset Value, or null when absent.

