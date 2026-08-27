---
module: HostTgbtc
source: ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp
source-commit: 06d4c50
---

# HostTgbtc

Binds the tgBTC bridge (Teleport BTC) to the engine: the arity-1 host fn tgbtc_mint_data derives a mint record from the consumed set, four arity-1 log readers parse single event-log blocks (mint 0x77A80EF3, burn, new key 0x27756729, dkg completed 0x453443A6), and the tgbtc_deploy_absorb shaper is registered in host_shapers to fold child contract deploys into the produced block. Core invariants: the log readers return Null for unreadable or short bodies instead of faulting, tgbtc_mint_data returns Null when the mint head 0x3F781D24, the success log, or the jetton_mint piece is missing, and burn tolerates truncated addresses as addr_none while mint and new key treat an extern address as a parse failure. Bitcoin txids render as fixed 64-character little-endian hex and pubkeys as minimal hex, renderings the rule language cannot express.

## class mch::MintLog

- decl: `ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:101`

Parsed tail of a mint event log: coins amount, recipient account, 256-bit txid, and an ok flag.

Default construction leaves ok false with the fields unset, and parse_mint_log fills the record stepwise, setting ok only after the txid read succeeds, so a partially filled record always carries ok false and callers discard it. Values move in and out by move construction; no block state is owned.

## class mch::LogCursor

- decl: `ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:71`

An opened event-log body positioned past its 32-bit opcode, with an ok flag.

Holds a body context plus ok, and open_log is the only producer; every failure stage (non-block argument, unreadable body, open failure, or fewer than 32 opcode bits) leaves ok false. The opcode bits themselves are skipped unchecked, since matching already happened when the host fn was scheduled.

## MintLog

`void MintLog()`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:101`
- callers: mch::parse_mint_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:109)

Default constructor leaving ok false.


## MintLog

`void MintLog(MintLog &&)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:101`
- callers: mch::parse_mint_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:112); mch::parse_mint_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:116); mch::parse_mint_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:121); mch::parse_mint_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:125); mch::parse_mint_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:129)

Move constructor used to return the parse result.


## parse_mint_log

`MintLog parse_mint_log(vm::CellSlice & cs)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:108`
- callers: mch::tgbtc_mint_data (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:177); mch::tgbtc_mint_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:216)
- calls: `mch::MintLog::MintLog`, `mch::account_id_of`, `mch::load_address_py`, `mch::load_coins_py`

Parses the coins amount, recipient address, and 256-bit txid from a mint log tail.

Each step failure returns early with ok false: a coins error, an address error, a recipient that is not an account (account_id_of flags extern strings), or missing 256 bits or a null txid. On success amount holds the coins value, recipient an account value, and txid the integer the callers render as a bitcoin txid.

## tgbtc_mint_data

`EvalResult tgbtc_mint_data(BuildEnv &, const std::vector<Value> & args)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:136`
- callers: none found in parsed TU set
- calls: `mch::Value::Value`, `mch::Value::make_account_none`, `mch::Value::make_bool`, `mch::Value::make_int`, `mch::Value::make_obj`, `mch::Value::make_str`, `mch::Value::null`, `mch::account_from_opt`, `mch::block_body`, `mch::block_msg`, `mch::data_field`, `mch::decode_consumed_or_none`, `mch::first_call`, `mch::open_body`, `mch::parse_mint_log`, `mch::rt_fault`, `mch::rt_ok`, `mch::txid_le_hex`

Builds the nine-field mint record from the consumed set's mint head, success log, and jetton_mint.

The first argument must be a List or the call faults; a failed decode, a missing head call (opcode 0x3F781D24), a missing success log (0x77A80EF3), or a missing jetton_mint block each return Null so the matcher can reject the candidate. The sender comes from the head's message source and the teleport contract from the success log's message source; the log tail goes through parse_mint_log and any failure there is Null as well. The recipient wallet comes from the jetton_mint's to_jetton_wallet field, passed through when already an account, parsed when a string, and account_none otherwise. The emitted record always carries success true and crippled false; Null is the only failure mode.


## tgbtc_mint_log

`EvalResult tgbtc_mint_log(BuildEnv &, const std::vector<Value> & args)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:211`
- callers: none found in parsed TU set
- calls: `mch::Value::make_int`, `mch::Value::make_obj`, `mch::Value::make_str`, `mch::Value::null`, `mch::open_log`, `mch::parse_mint_log`, `mch::rt_ok`, `mch::txid_le_hex`

Reads a single mint event log into amount, recipient, and bitcoin txid.

Opens the log and parses the tail; any open or parse failure returns Null. The txid is rendered by txid_le_hex as fixed 64-character little-endian lowercase hex.

## tgbtc_burn_log

`EvalResult tgbtc_burn_log(BuildEnv &, const std::vector<Value> & args)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:230`
- callers: none found in parsed TU set
- calls: `mch::Value::make_account_none`, `mch::Value::make_int`, `mch::Value::make_obj`, `mch::Value::null`, `mch::account_id_or_none`, `mch::load_address_py`, `mch::load_coins_py`, `mch::open_log`, `mch::rt_ok`

Reads a burn event log with tolerant address handling.

The coins amount is strict: an error there returns Null, and only the address loads are tolerant. The first address-load error keeps the already-parsed sender and leaves the pegout account_none, and account_id_or_none maps extern addresses to account_none rather than rejecting, matching BurnEvent's tolerance for truncated addresses; partial results are kept deliberately with no rollback.

## tgbtc_new_key_log

`EvalResult tgbtc_new_key_log(BuildEnv &, const std::vector<Value> & args)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:258`
- callers: none found in parsed TU set
- calls: `mch::Value::make_int`, `mch::Value::make_obj`, `mch::Value::null`, `mch::account_id_of`, `mch::hex_min`, `mch::load_address_py`, `mch::load_coins_py`, `mch::open_log`, `mch::rt_ok`

Reads a new-key event log into amount, minimal-hex pubkey, and pegout address.

After the strict coins read, a missing or null 256-bit pubkey returns Null, and the pegout address must parse and be an account: an extern pegout flags failure through account_id_of and the whole record becomes Null. The pubkey renders through hex_min, minimal lowercase with zero as a bare 0.

## tgbtc_dkg_completed_log

`EvalResult tgbtc_dkg_completed_log(BuildEnv &, const std::vector<Value> & args)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:289`
- callers: none found in parsed TU set
- calls: `mch::Value::make_int`, `mch::Value::make_obj`, `mch::Value::null`, `mch::hex_min`, `mch::open_log`, `mch::refint_u64`, `mch::rt_ok`

Reads a dkg-completed event log into timestamp and minimal-hex pubkey.

Requires 64 bits of timestamp widened through refint_u64 and then a further 256-bit pubkey; either shortfall returns Null. The pubkey renders through hex_min.

## tgbtc_deploy_absorb

`void tgbtc_deploy_absorb(Block * produced, const ShaperMatch & m)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:311`
- callers: none found in parsed TU set
- calls: `mch::Block::merge_blocks`

Shaper that merges the consumed blocks' child contract deploys into the produced block.

Collects every ContractDeploy child of each consumed block and merges them into the produced block with merge_blocks; with nothing collected it does nothing. It runs as the shaper hook after wrapper-merge, per the host_shapers registry entry.

## txid_le_hex

`std::string txid_le_hex(const td::RefInt256 & v)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:33`
- callers: mch::tgbtc_mint_data (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:187); mch::tgbtc_mint_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:223)

Renders a 256-bit integer as 64-character little-endian lowercase hex.

The integer is exported into 32 zero-padded little-endian bytes, then hex-encoded with a fixed lowercase alphabet.

## hex_min

`Value hex_min(const td::RefInt256 & v)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:47`
- callers: mch::tgbtc_new_key_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:283); mch::tgbtc_dkg_completed_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:305)
- calls: `mch::Value::make_str`

Renders an integer as minimal lowercase hex with no leading zeros, and zero as a bare 0.


## account_id_of

`Value account_id_of(const Value & v, bool & ok)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:53`
- callers: mch::parse_mint_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:119); mch::tgbtc_new_key_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:277)
- calls: `mch::Value::Value`, `mch::Value::is_null`, `mch::Value::make_account_none`, `mch::Value::null`

Passes accounts through, maps null to account_none, and flags any other value by clearing ok.

An extern-address string is the flagging case, telling the caller to treat the parse as failed.

## account_id_or_none

`Value account_id_or_none(const Value & v)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:65`
- callers: mch::tgbtc_burn_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:244); mch::tgbtc_burn_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:247)
- calls: `mch::Value::Value`, `mch::Value::make_account_none`

Passes accounts through and maps everything else, including extern addresses, to account_none.


## LogCursor

`void LogCursor()`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:71`
- callers: mch::open_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:77)

Default constructor leaving ok false.


## LogCursor

`void LogCursor(LogCursor &&)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:71`
- callers: mch::open_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:80); mch::open_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:84); mch::open_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:88); mch::open_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:92); mch::open_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:96)

Move constructor used by open_log's returns.


## open_log

`LogCursor open_log(const std::vector<Value> & args)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:76`
- callers: mch::tgbtc_mint_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:212); mch::tgbtc_burn_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:231); mch::tgbtc_new_key_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:259); mch::tgbtc_dkg_completed_log (ton-index-worker/ton-mch-engine/src/host/HostTgbtc.cpp:290)
- calls: `mch::LogCursor::LogCursor`, `mch::as_block`, `mch::block_body`, `mch::open_body`

Opens the block argument's log body past the opcode and reports whether it succeeded.

The argument must be a Block value with a body that opens cleanly and still holds 32 opcode bits, which are skipped without inspection; any failure returns a cursor with ok false. Every log-reader host fn starts here and turns ok false into a Null result.
