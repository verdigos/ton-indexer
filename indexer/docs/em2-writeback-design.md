# EM2 — write-back: mch Action rows into the emulator's Redis MULTI (design)

> Status: DESIGN, **revision 2** — Q1-Q6 resolved by user + lead, adversarial review folded in (no
> blockers; contract claims verified line-for-line, Go decode failure reproduced empirically).
> Scope = program plan `emu-integration-plan.md` §3 "EM2".
> Baseline = commit `3444c22` on `merge-v13` (EM1.5 as-built) plus the uncommitted EM1.5 fixes in
> `EmuShadow.cpp` / `EmuTypes.h` / `TraceScheduler.{h,cpp}` / `main.cpp` at the time of writing.
> Companions: `em1-design.md` (§§11-20 = EM1.5 as-built), `em2-tier2-design.md` (the other half of
> EM2 — tier-2 lookup wiring; its §8 failure invariant meets this doc's §7 at Q4),
> `mch-cpp-prod-design.md` (Phase G), `mch-cpp-plan.md` §1 (gate set), §10 (standing hazards).
>
> This doc covers the WRITE-BACK only. Tier-2 wiring is a pre-EM2 blocker
> (`emu-integration-plan.md` §1.4b) designed separately; without it stonfi/Tonco/dedust degrade and
> nominator/jvault emit nothing, so the §8 A/B numbers are only meaningful once it has landed.
>
> Every factual claim below carries a `file:line`. Paths are relative to
> `/home/verdigos/dev/ton-indexer-private/` unless absolute.
>
> **Verification for this phase stays COMPILE-ONLY plus offline twin gates** (§9) — no emulator
> launch, no live Redis write from our binary, until the A/B in §8 has run.

---

## 0. What EM1.5 already hands us, and what is left

EM1.5 delivers, per trace emission, a fully-built `std::vector<mch::Action>` to the sink continuation
that runs the insert, with the insert gated on the response:

- The decorated sink builds an `EmuTraceView` (Ref/POD walk, cannot throw) and sends it to
  `EmuClassifierActor::classify` with a `td::Promise<EmuClassifyResult>`
  (`ton-index-worker/ton-mch-engine/src/emu/EmuShadow.cpp:110-158`).
- The actor converts, classifies and runs `build_action` **inside** the arena's lifetime
  (`ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:118-127`), scrubs arena `Block*`
  values (`:65-77`), and answers exactly once (`:304`).
- The continuation currently just calls `inner(...)` — the literal EM2 seam is
  `EmuShadow.cpp:151-153` (`// EM2 moves R.ok().actions into the insert here`).

So EM2 is **not** "make classification happen". It is four things:

1. `mch::Action` → msgpack bytes that `ton-index-go` decodes (§2, §3).
2. A payload type + the widened insert seam, and where the writes land inside the existing MULTI
   (§4).
3. An up-front admission work cap, because the 200 ms deadline bounds queue wait only (§5).
4. An explicit answer for "trace with no actions" (§6, §7), plus the A/B that must precede the flip
   (§8).

---

## 1. What the MULTI contains today, per emission

`TraceInserter::start_up` (`ton-index-worker/ton-trace-emulator/src/TraceInserter.cpp:46-270`).
Two commands run **outside** the transaction, on the same connection, synchronously:
`hlen(trace_key_)` (`:56`) and per-node `hget` inside `load_existing_node`/`delete_db_subtree`
(`:291`, `:326`). Everything below is queued into the MULTI and flushed by `exec()` at `:260`:

| command | where | note |
|---|---|---|
| `HDEL <trace_key> <node keys…>` | `:121-123` | only when a re-emission supersedes emulated nodes |
| `ZREM <wc:HEXADDR> <trace_key>:<msg_b64>` | `:125-127` | the **bare-address** ZSET, not `_aai` |
| `ZADD <wc:HEXADDR> <lt> <trace_key>:<msg_b64>` | `:146` | one per inserted node |
| Lua CAS on `account_{finalized,confirmed}:<addr>` + `PUBLISH new_account_state` | `:187-210` | per committed account |
| `PUBLISH new_finalized_txs` / `new_confirmed_txs` | `:213-219` | when the emission has committed txs |
| `PUBLISH new_commited_txs` | `:221-223` | ditto |
| `HSET <trace_key> <field…>` — all node blobs, all interface blobs, `root_account_code_hash`, `root_node`, `depth_limit_exceeded`, otel propagation fields | `:132-133`, `:142`, `:165`, `:230-232`, `:241-247`, batched into ONE `hset` at `:248-250` | |
| `SET tr_in_msg:<ext_hash_b64> <trace_key>` + `EXPIRE 600` | `:252-253` | |
| `PUBLISH new_pending_trace` (only if ≥1 emulated node) / `PUBLISH new_trace` | `:255-258` | |

Two early returns skip the whole MULTI: the >1000-field guard (`:59-65`) and `flattened_trace.empty()`
(`:111-116`). **Anything EM2 adds must sit after those**, so both keep skipping cleanly.

`RedisInsertManager::insert` creates one `TraceInserter` actor per trace with
`redis_.transaction(false, false)` (`:343-350`, constants at `:10-11`) — un-pipelined, shared
connection, so EM2's added commands serialize against other inserters exactly as the current ones do.

**What Python adds today, from a different process** (`indexer/indexer/events/pendings.py`):

- `HSET <trace_key> actions gzip(msgpack([a.to_dict() …]))` (`:243-245`, gzip level 1)
- `ZADD _aai:<account> <trace.start_lt> "<trace_key>:<action_id>"`, one member per (action, account)
  (`:257-263`, `:270-271`)
- `PUBLISH referenced_accounts "<account>;<trace_key>"` for every account that appears in an action
  but is **not** a transaction account (`:266-267`, `:274-275`)
- optional otel fields + a response-channel publish (`:248-255`)

The `_aai` ZADDs are Python-only (the C++ ZADD at `TraceInserter.cpp:146` goes to the bare address
key) — EM2's writer inherits none of them and must add all three.

`referenced_accounts` is load-bearing, not decoration: `ton-trace-ttl-tracker` subscribes it
(`ton-trace-ttl-tracker/main.go:1062`), merges the account into its per-trace address set
(`:372-386`), and uses exactly that set at expiry to `ZREM` the trace's members out of
`_aai:<addr>` (`:564-573`). Stop publishing it and every `_aai` entry for a
non-transaction account leaks forever.

The payload format is not merely conventional, it is a **remote-kill surface**: the subscriber does
`tokens := strings.Split(data, ";")` then indexes `tokens[1]` with no length check (`:378-381`); a
payload without a `;` panics that goroutine; there is **no `recover()` anywhere in the module** and
the only deferred call on that goroutine is `rtt.wg.Done()` (`:341`) — so the panic takes the whole
ttl-tracker process down. Anyone who can publish on that channel can kill the service. Our writer
must never emit a malformed payload, and the account string must be built, not interpolated from
anything that could contain a separator.

---

## 2. The wire contract

### 2.1 Presence: absent ≠ null, and Python omits a lot

`Action.to_dict()` is `self.__dict__.copy()` minus `_sa_instance_state`
(`indexer/indexer/core/database.py:665-670`). `Action` has no custom `__init__`; it uses SQLAlchemy's
declarative constructor plus plain attribute assignment, and nothing iterates `__table__.columns`, so
**a column that was never assigned is simply not a key in the map** — reading it returns `None`
without materialising it. Confirmed empirically by the research pass against the repo venv.

On the Go side that is harmless: unknown wire keys are skipped
(`decode_map.go:327-353` in `github.com/vmihailenco/msgpack/v5@v5.4.1`), and absent keys leave the
struct field at its zero value (nil pointer / `""` / `false` / nil slice). Present-as-nil is
equivalent for pointer fields. **So EM2 may omit any field it cannot fill**, and should — it matches
Python and costs bytes nothing.

Python's per-action key set on the emulated path is:

- from `_base_block_to_action` (`indexer/indexer/events/blocks/utils/block_tree_serializer.py:133-147`):
  `trace_id, type, action_id, tx_hashes, start_lt, end_lt, start_utime, end_utime, success,
  mc_seqno_end, finality, value_extra_currencies`, plus `accounts`
- from `block_to_action` (`:1405-1409`): `trace_end_lt, trace_end_utime, trace_external_hash,
  trace_external_hash_norm, trace_mc_seqno_end`; then `tx_hashes` is re-assigned at `:1595`
- from `serialize_blocks` (`:1661`): `parent_action_id` (may be `None`); `ancestor_type` only for
  child actions of non-`v1_ops` parents (`:1665-1673`)
- from the per-type `_fill_*` (`:1410-1578`): the directional fields and the one `*_data` composite
- from `pendings.py:236-239`: `trace_id = None`, `trace_external_hash = trace.external_hash`,
  `trace_external_hash_norm = trace_key` — unconditional in practice on this path

### 2.2 Stringify: the exclusion set is TOP-LEVEL ONLY

`convert_numerics_to_strings` (`database.py:147-174`) passes the excluded keys through untouched at
the top level and, for every other key, recursively converts `int`/`float` (but **not** `bool`) to
`str` — including inside the composite dicts. Excluded set (`:669`): `start_lt, end_lt, start_utime,
end_utime, opcode, trace_start_lt, finality, trace_end_lt, trace_end_utime, mc_seqno_end,
trace_mc_seqno_end`.

Most numeric leaves reaching `Action` on this path are native Python `int` (msgpack decode →
`int` all the way; the only `Decimal` conversion, `ton_utils.py:125-129`, belongs to the SQL insert
path and never runs here), so `str(v)` is a plain base-10 digit string.

**W2 CORRECTION — not all of them are ints, and the exponent hazard is real.** The parity gate found
`float` leaves in `nft_transfer_data.price` and `.nft_item_index` on real corpus traces. For those,
`str(v)` follows `repr(float)`, which switches to exponent notation once the decimal exponent leaves
`(-4, 16]` — `str(1e16) == '1e+16'`. Both writers must therefore implement `repr(float)`, not a
generic shortest-round-trip formatter; see the `Float` row in §2.7.

Go's decoder is **strict in both directions**: a msgpack str into `*uint64`/`*int64`/`*int32` errors
(`decode_number.go:90-120`, error return at `:119`; the `int64` twin at `:162`), and a msgpack int
into `*string` errors (`decode_string.go:10-32`, error return at `:31`). And a single field-level
type error aborts the whole
`msgpack.Unmarshal(decompressed, &actions)` (`ton-index-go/index/emulated/models.go:826-829`), which
makes `ConvertHSet` return an error, which makes `FillFromRawData` **drop the entire trace** with a
log line (`ton-index-go/index/crud/pending.go:181-185`). There is no per-field or per-action
tolerance.

### 2.3 Field-by-field map

`mch::Action` (`ton-index-worker/ton-mch-engine/src/ActionBuild.h:19-33`) → wire key → Go field
(`ton-index-go/index/emulated/models.go:530-601`). "int" = msgpack integer, "str" = msgpack string.

| wire key | mch source | wire type | Go field / type | notes |
|---|---|---|---|---|
| `action_id` | `Action::action_id` (`ActionBuild.cpp:559-570`) | str | `ActionId string` | sha256(root msg_hash\|tx_hash + btype), base64 — same algorithm as Python `_calc_action_id` (`block_tree_serializer.py:110-119`) |
| `type` | `Action::type` (`ActionBuild.cpp:557`, several fills override) | str | `Type string` | |
| `success` | `Action::success` (`:558`, overridden by 3 fills) | bool | `Success bool` | never stringified (`database.py:162` excludes `bool`) |
| `tx_hashes` | `Action::tx_hashes` (`:572-589`) | array of str | `TxHashes []string` | base64 tx hashes; C++ set is sorted, Python's is not — order is not compared by any consumer |
| `accounts` | `Action::accounts` (`:597-621`) | array of str | `Accounts []string` | raw `wc:HEXUPPER`; Go passes it through to JSON with **no** normalisation (`models.go:943-946`, `index/models/convert.go:23-25`) |
| `start_lt`, `end_lt` | `Action::start_lt/end_lt` (`:590-591`) | **int** | `*uint64` | in Python's exclusion set |
| `start_utime`, `end_utime` | `:592-593` | **int** | `*uint32` | ditto; values fit u32 |
| `mc_seqno_end` | `:594` | **int** | *(no Go field)* | Python writes it; Go ignores it. Keep for parity, cost ~1 key |
| `opcode` | `Action::opcode` (Value Int) | **int** | `*uint32` | Python masks to unsigned (`tree_utils.py:45-49`), so always in `[0, 2^32)`. Excluded from stringify |
| `finality` | not in `mch::Action` — see §2.5 | **int** | `models.FinalityState` (`uint8`) | Python = trace-level `min(tx.finality)`; wire values come from the emulator's own enum `{0,1,2}` (`TraceEmulator.h:15-19`), which is what Go's `FinalityStatePending/Confirmed/Finalized` means (`ton-index-go/index/models/types.go:113-118`). Python's 4-value enum (`database.py:140-144`, `signed=2`, `finalized=3`) never puts a 3 on the wire because the min is taken over emulator-supplied values |
| `value`, `amount` | `Action::value/amount` via `av_amount` (`ActionBuild.cpp:49-56`) | **str** | `*string` | decimal string of the 256-bit int |
| `source`, `source_secondary`, `destination`, `destination_secondary`, `asset`, `asset_secondary`, `asset2`, `asset2_secondary` | `av_addr` (`ActionBuild.cpp:40-45`) | str | `*string` | canonical `wc:HEXUPPER`, matching `AccountId.as_str()` (`indexer/indexer/events/blocks/utils/ton_utils.py:94-97`) and the engine's `normalize_raw_address` (`SchemaTraceLoader.cpp:110-116`) |
| `trace_external_hash_norm` | not in `mch::Action` | str | `*string` | = the trace key = `base64(ext_in_msg_hash_norm)` = `EmuTraceView::trace_id` (`EmuShadow.cpp:62`, same value as `TraceInserter.cpp:55`). **Must be written**: Go only surfaces it when it differs from `trace_external_hash` (`models.go:930`) |
| `extra` | `Action::extra` | map str→str | `map[string]interface{}` | only `tgbtc_*` fills set it |
| the 13 `*_data` composites present in `mch::Action` | see §2.4 | map | typed structs | |

**Omit deliberately** (Go clobbers them right after decode, `models.go:830-836`): `trace_id`,
`trace_external_hash`, `trace_end_lt`, `trace_end_utime`, `trace_mc_seqno_end`. Also omit
`trace_start_lt` (never assigned by Python either — it is not a column, only a defensive member of
the exclusion set) and `value_extra_currencies` (Python always writes `{}` on this path because
`Message.value_extra_currencies` is never set by `event_deserializer.py:12-34`; Go has no field for
it and hardcodes `map[string]string{}` at `models.go:975`). Omitting them also removes eleven chances
to get a wire type wrong.

### 2.4 Composite payloads: what mch can fill

mch's `Action` carries 13 composite fields (`ActionBuild.h:29-32`) against Python's ~50. The gap is
not a serializer gap — it is the ported-matcher gap. Buckets:

**A. Filled by mch, byte-comparable today** (the 19 composite btypes fired by `fill_action`,
`ActionBuild.cpp:521-553`): `ton_transfer_data`, `jetton_transfer_data`, `jetton_swap_data`,
`nft_transfer_data` (+ `nft_listing_data`, which `fill_nft_transfer` also sets),
`dex_deposit_liquidity_data`, `dex_withdraw_liquidity_data`, `staking_data`, `evaa_withdraw_data`,
`vesting_send_message_data`, `layerzero_packet_data`, `jvault_stake_data`, `jvault_claim_data`,
`extra`.

**B. Matcher runs, fill missing → `build_action` returns false and the WHOLE row is dropped**
(`ActionBuild.cpp:595`): `change_dns`/`renew_dns`/`delete_dns`, bare `dex_deposit_liquidity`
(stonfi v2 provide), bare `dex_withdraw_liquidity` (dedust/stonfi v2 withdraw),
`nominator_pool_withdraw_request`, `tonstakers_withdraw`/`tonstakers_withdraw_request`, and the
named skips `nft_purchase`/`dns_purchase` (`:545-550`). These are counted as `unported_btypes`
(`EmuClassifierActor.cpp:120-122`) — the counter already exists and is already in the 10 s line
(`:331`). **This bucket was the biggest EM2 risk to output quality**: the matcher fires, so the
protocol *is* recognised, and yet the action silently disappears from the pending payload.

**W6 STATUS — six of nine ported, and the bucket is INVISIBLE to the corpus.** Measured, not assumed:
all 33 matchers are runnable (zero `SKIP` lines), the classify dump's 227 produced blocks span 19
btypes that are *all* inside the ported fill set, and `build_action` therefore declines **0** rows
across the 165 fixtures. The bucket is empty in the corpus, so the A/B twin **cannot** gate it —
these fills are gated hermetically in `--actions-msgpack-test` instead, with the data field names
taken from the IR's own `out`/`switch` field lists and the column set from the matching Python
`_fill_*`. The bucket is emphatically *not* empty in production: it is DNS renewals, stonfi v2
provide/withdraw, dedust withdraw and nominator/tonstakers withdrawals.

| btype | producing matcher(s) | W6 |
|---|---|---|
| `renew_dns` | `change_dns` | **ported** |
| `nominator_pool_withdraw_request` | `nominator_pool_withdraw` | **ported** — a payout present flips the type to `stake_withdrawal` |
| `tonstakers_withdraw` | `tonstakers_withdraw` | **ported** |
| `tonstakers_withdraw_request` | `tonstakers_withdraw` | **ported** |
| `dex_deposit_liquidity` (bare) | `stonfi_v2_provide_final`, `stonfi_v2_provide_partial` | **ported** |
| `dex_withdraw_liquidity` (bare) | `dedust_withdraw`, `stonfi_v2_withdraw`, `stonfi_v2_withdraw_refund` | **ported** |
| `nft_purchase` | `getgems_nft_purchase` | **ported** — the former named skip, **reversed by the lead in W6**. The old note left it out for want of an A/B gate; that reasoning inverts under EM2, because the matcher runs and Python emits the row, so omitting it does not avoid a phase-1 diff — it *manufactures* one in `real-mismatch`, on GetGems traffic that is plentiful live |
| `change_dns`, `delete_dns` | `change_dns` | **BLOCKED, deferred-approved** — both need a `change_dns_record_data` column `mch::Action` does not have. Adding it also means extending `scrub_arena_refs` (`emu/EmuClassifierActor.cpp`), owned by the tier-2 wave; queued for when that file reopens. Go already has the field (`ChangeDnsRecordData`), so this is a C++-side gap only. A hermetic assertion pins `change_dns` as still-declining, so the stale expectation fails loudly the day the column lands |
| `dns_purchase` | *none* | bucket **C**, not B: no matcher in `ir/mch_ir.json` produces it. §2.4 previously listed it under B — a doc error |

The twin's `PORTED_BTYPES` (`twins/py_actions.py`) was widened to match. That changes no output today — none of these btypes fire on the corpus — but it keeps the twin correct-by-construction: the day a fixture carrying one is added, Python emits its row instead of skipping it while C++ emits one, which would read as a bogus `cpp-only-row` rather than the parity it actually is.

**C. Protocol not ported at all** (no matcher in `ir/mch_ir.json`): `nft_mint_data`,
`tonco_deploy_pool_data`, `multisig_*` (3), `vesting_add_whitelist_data`, `evaa_supply_data`,
`evaa_liquidate_data`, `coffee_create_pool_data`, `coffee_staking_{deposit,withdraw}_data`,
`layerzero_send_data`, `layerzero_dvn_verify_data`, all 12 `cocoon_*`, `wallet_v5_extension_data`.
These are the plan's "protocol-not-in-mch" A/B bucket (§8) and are EM3 work.

### 2.5 What mch structurally cannot fill

| Python attribute | status | consequence |
|---|---|---|
| `trace_id`, `trace_external_hash`, `trace_end_lt`, `trace_end_utime`, `trace_mc_seqno_end` | absent from `mch::Action`; Go recomputes them | none — omit |
| `trace_external_hash_norm` | absent from `mch::Action`, but the **classifier has the value** (`EmuTraceView::trace_id`) | fill from the view, not from the Action |
| `finality` | absent | fill from the view: `min` over `EmuTxRef::finality` (`EmuTypes.h:59`), matching Python's `min(tx.finality)` |
| `value_extra_currencies` | absent everywhere (`mch-cpp-plan.md:307-308`, "§10 residual"; `ActionBuild.cpp:505-506`) | none on the wire — Go has no field |
| `parent_action_id` | absent — `serialize_blocks`' child recursion is out of scope (`ActionBuild.cpp:27`) | decoded by Go (`models.go:560`) but never read by `ToRawAction` (`:928-1299`, the full body); harmless |
| `ancestor_type` | absent, same reason | **conditionally** load-bearing: `GetActions` skips an action whose `ancestor_type` intersects the requested set (`ton-index-go/index/crud/pending.go:149-151`) — that is the child-suppression mechanism. mch emits **no child actions at all** (only surviving top-level post-pass blocks, `EmuClassifierActor.cpp:116-118`), so an empty `ancestor_type` suppresses nothing that exists. It becomes a bug the moment the child recursion is ported: port `ancestor_type` in the same wave or `GetActions` starts leaking children |

### 2.6 FINDING: Python's own payload is undecodable for some action types

Because the exclusion set is top-level only (§2.2), every integer **inside** a composite is
stringified — while several Go composite fields are non-string:

| composite | field | Python emits | Go expects | file:line |
|---|---|---|---|---|
| `jvault_stake_data` | `period` | str (`period: int`, `indexer/indexer/events/blocks/jvault.py:80,123`; assigned raw at `block_tree_serializer.py:828`) | `*int64` | `models.go:411-415` |
| `layerzero_packet_data` | `src_eid`, `dst_eid`, `nonce` | str (`packet_data.__dict__` passthrough, `block_tree_serializer.py:1320,1342,1351`) | `*int32`, `*int32`, `*int64` | `models.go:449-457` |
| `multisig_create_order_data` | `creator_index`, `expiration_date` | str (`:870-871`) | `*int64` | `models.go:349-357` |
| `multisig_approve_data` | `signer_index`, `exit_code` | str (`:884-885`) | `*int64`, `*int32` | `models.go:359-362` |
| `multisig_execute_data` | `expiration_date`, `approvals_num` | str (`:897-898`) | `*int64` | `models.go:364-371` |
| `layerzero_send_data` | `send_request_id`, `native_fee`, `zro_fee` | str (`:1311,1315-1316`) | `*uint64` | `models.go:438-447` |
| `layerzero_dvn_verify_data` | `nonce` | str (`:1355`) | `*int64` | `models.go:459-466` |
| `cocoon_worker_payout_data` | `worker_state` | str | `*int64` | `models.go:468-474` |
| `cocoon_unregister_proxy_data` | `seqno` | str | `*int64` | `models.go:494-497` |

Any one of these in a pending trace makes `msgpack.Unmarshal` fail → `ConvertHSet` errors →
**the whole trace vanishes from the pending endpoints**, not just the action
(`models.go:826-829` → `pending.go:181-185`). The mechanism is certain from the code, and the review
pass reproduced the decode failure empirically. The live frequency is still unmeasured and should be
checked against prod Redis before anyone treats it as urgent (it is a pre-existing Python↔Go defect,
not something EM2 introduces).

**There is no configuration escape.** Both decode sites are bare `msgpack.Unmarshal`
(`models.go:791`, `:826`); no `msgpack.NewDecoder`, `DisallowUnknownFields`,
`UseLooseInterfaceDecoding` or `SetCustomStructTag` appears anywhere in `ton-index-go`, and no
msgpack struct tag in the repo uses a `,string` flag — the `,string` hits under `index/models/` are
all **json** tags (e.g. `index/models/nft.go:8`, `index/models/traces.go:22`). So the mismatch cannot
be papered over on the reader side by a tag or an option; it is structural.

**DECIDED (Q1, user + lead): diverge — emit integers.** Of the nine, exactly two composites are in
mch's ported set: `jvault_stake_data.period` and `layerzero_packet_data.{src_eid,dst_eid,nonce}`.
EM2 emits those four as **msgpack integers**, deliberately departing from Python. Rationale:
"byte-parity" here would mean reproducing a payload the consumer rejects, and the whole point of the
write-back is that the trace is visible *with* its actions. The A/B carries this as a named
`wire-type-divergence` bucket (§8) rather than a mismatch, and the follow-up is a one-line Python
change (a nested-exclusion rule) so both writers converge — at which point the remaining seven
fields stop dropping traces too.

### 2.7 `Value` → msgpack

`Value` (`ton-index-worker/ton-mch-engine/src/Value.h:47-79`) is the carrier for every non-scalar
Action field. The writer maps:

| VType | msgpack | rationale |
|---|---|---|
| `Null` | **top level: omit the key; inside a composite: msgpack nil** | see the two-level rule below |
| `Bool` | bool | `database.py:162` never stringifies bools |
| `Int` | str (`num->to_dec_string()`) | Python stringifies every non-excluded int; **except** the §2.6 keys and the excluded top-level keys, which stay int |
| `Str` | str | |
| `Bytes` | str = `td::base64_encode(v.str)` | Python holds these as base64 `str` already (`indexer/indexer/events/blocks/jettons.py:91-94,163-166`); the C++ side holds the raw BOC bytes (`ton-index-worker/ton-mch-engine/src/parse/ParseJettons.cpp:59-67,93-97`) |
| `Amount` | str of `num`; `make_amount_none()` → as `Null` | `av_amount` already unwraps to `Int` for the ported fills (`ActionBuild.cpp:49-56`) |
| `Account` | str (canonical); `addr_none` → as `Null` | mirrors `AccountId.as_str()` returning `None` |
| `Asset` | jetton → str; TON → as `Null` | already normalised by `av_addr` (`ActionBuild.cpp:40-45`) |
| `List` | array | |
| `Dict` / `Obj` | map, insertion order | the composite payloads |
| `Float` | str, Python `repr(float)` rules (see below) | **W2 CORRECTION**: ported fills *do* produce Floats — `nft_transfer_data.price` and `.nft_item_index` carry them in the corpus. Plain `std::to_chars` is WRONG here (`1037` vs `1037.0`, `1.03e+11` vs `103000000000.0`); both Go fields are `*string` (`models.go:219,225`), so the difference is invisible to the decoder but changes the text an API consumer reads, and Python is the incumbent. Still **counted** |
| `Cell` | str = base64 of `td_boc_serialize` | should never reach an Action; count it like the `Float` case |
| `Block` | impossible — already scrubbed | `EmuClassifierActor.cpp:44-63` nulls and counts these |

**The presence rule has two levels, and conflating them is the trap.** "Absent" and "present as nil"
are the same thing to Go (§2.1), but they are *not* the same thing to a decoded-map comparator, so
the writer must reproduce Python's split exactly:

- **Top level → omit.** `Action` has no custom `__init__` and nothing iterates `__table__.columns`, so
  a field a `_fill_*` never assigned is simply not a key in `to_dict()` (§2.1). A `Null` /
  `addr_none` / TON-`Asset` / `Amount(None)` Action field is therefore dropped from the map.
- **Inside a composite → nil.** A composite is a **literal dict** built by its fill, with every key
  present and the unfilled ones set to `None` — `_fill_jetton_transfer_action` assigns all seven
  `jetton_transfer_data` keys including `'comment': None`
  (`indexer/indexer/events/blocks/utils/block_tree_serializer.py:195-204`). A composite's arity is
  its fill's, not its filled subset, so the writer keeps every key and packs the empty ones as nil.

That makes W2's comparator a **strict decoded-map equality** with no nil≡absent normalisation, which
is why the split is worth the extra branch: the gate that carries all the contract risk gets no
tolerance rule that could mask a real omission. (Resolved by the lead during W1; the writer's
`omitted()` is top-level-only and `pack_fields` keeps every key —
`ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp`.)

Two rendering rules from the harness must **not** be reused: `render_value`'s prefixed text form
(`ton-index-worker/ton-mch-engine/src/BuildDriver.cpp:52-117`, `"str:"`, `"amount:"`, `"asset:"`) and
the BOC-field root-hash substitution `is_boc_field`/`boc_field_cellhash`
(`Value.h:116-130`, `Value.cpp:167-223`). Both are twin-diff devices, explicitly documented as such
(`Value.h:118-126`). The real payload bytes are present in the `Value` and must be base64'd verbatim.

**Encoding mechanics.** The map must be built dynamically (`msgpack::packer::pack_map(n)` + explicit
key/value packs), not with `MSGPACK_DEFINE_MAP` on a fixed struct, precisely because the key set
varies per action and omission is the contract. `msgpack-cxx` is already a dependency of both the
emulator target (`ton-index-worker/ton-trace-emulator/CMakeLists.txt:44`) and mch
(`ton-index-worker/ton-mch-engine/src/TraceLoader.cpp:7`); nothing new is linked.

**Key order / "byte-for-byte".** The plan says a C++ writer "must reproduce it byte-for-byte"
(`emu-integration-plan.md:83`). That is not achievable and not needed: Python's map order is
`__dict__` insertion order, which depends on the order `_fill_*` happens to assign attributes, and
msgpack maps are order-insensitive to the Go decoder. **The parity bar for EM2 is decoded-map
equality**, per key, with the §2.6 divergence listed. See the Corrections section.

### 2.8 Compression

Python gzips at level 1 (`pendings.py:244`). Go sniffs the two-byte gzip magic and, on a miss,
**passes the bytes through uncompressed** (`models.go:750-765`), so both forms work.

Do **not** use `td::gzencode`: `td::Gzip::init_encode` calls `deflateInit2(..., windowBits=15, ...)`
(`ton-index-worker/external/ton/tdutils/td/utils/Gzip.cpp:46-50`), i.e. **zlib** framing
(`0x78 0x9c`), not gzip (`0x1f 0x8b`). Go's sniff would miss, hand the zlib bytes to
`msgpack.Unmarshal` as if they were msgpack, and drop the trace. (Its *decoder* is fine —
`inflateInit2(..., MAX_WBITS + 32)` at `:61` auto-detects both — which is exactly why this asymmetry
is easy to miss.)

**DECIDED (Q2): ship uncompressed msgpack.** Correct by construction, one less moving part, and it
removes the most expensive half of the serialization budget in §3. Real gzip framing (`deflateInit2`
with `15 + 16`, ~25 lines against zlib, already linked via `TD_HAVE_ZLIB`) is added later only if the
Redis memory delta measured in the A/B justifies it.

---

## 3. Where serialization runs — CHOSEN: in the classifier actor, ship BYTES

**Chosen** (unchallenged in review): `EmuClassifierActor::classify` builds the msgpack blob and the index rows before
answering, and `EmuClassifyResult` carries an `EmuActionPayload` of plain `std::string`s instead of
`std::vector<mch::Action>`.

Why:

1. **It deletes a use-after-free class rather than documenting it.** Today the result must hand the
   anchor back and relies on member declaration order to outlive the rows, with the comment
   conceding that a caller who moves `actions` out "is back to a use-after-free, which the ordering
   alone cannot prevent" (`EmuTypes.h:88-96`). If the result holds bytes, the anchor's obligation
   ends when `classify` returns; `EmuClassifyResult::anchor` can be **deleted**, and the `VType::Cell`
   hazard (`Value.h:76`) stops being an integration-point concern entirely.
2. **`TraceInserter` needs no mch dependency.** The payload is `std::string` + `vector<pair<string,
   string>>` + `vector<string>` + `int64`. No `Value.h`, no `ActionBuild.h`, no arena semantics in a
   file that runs blocking Redis round-trips.
3. **It keeps the "everything the anchor protects is read on one thread" invariant** that EM1.5 spent
   §12 establishing.

**Honest cost.** The classifier actor is a single serialization point for all traces
(`em1-design.md` §18), so this adds to `classify_us` for everyone, not just the one trace. Order of
magnitude: msgpack of a handful of small maps is tens of µs; gzip of a few KB is another 20-60 µs.
Against a ~100 µs average that is a real 20-60% increase in the critical section — which is why §2.8
ships uncompressed, and why `serialize_us` must be a separate field in the result and a
separate column in the 10 s line. The duty-cycle headroom is large: at the live-verified 9.5 traces/s
(`em1-design.md` §18), 150 µs per trace is ~0.15% of one thread, and even the pathological case where
*every* trace cost the observed 13.3 ms tail would be ~13%. The escape hatch is already designed
anyway: N classifier actors sharded by `hash(trace_id)` (`em1-design.md` Q9).

The deadline does not interact with serialization: **today** it is tested once at dequeue, before any
work (`EmuClassifierActor.cpp:204-209`), and serialization happens after classification, so it cannot
cause a shed — it only extends the tail the deadline never covered (`em1-design.md` §16.2). That
"tested once" phrasing goes **stale when tier-2 lands**, because the fixpoint loop re-checks the
deadline per round (`em2-tier2-design.md` §5.4); the invariant that survives is narrower and is the
one to rely on: *the deadline is only ever evaluated before work, never during it, so no partially
serialized or partially classified result can be discarded mid-flight.*

**Composed CPU budget, once tier-2 rounds exist** (S11, shared with the tier-2 design). Per-block
worst-case added latency is:

```
28 traces/block × (rounds × classify_us + serialize_us)
```

using the ~28 traces per block from `em1-design.md` §18. At the observed 13.3 ms tail with the
tier-2 round cap of 4 (`em2-tier2-design.md` §4.4), that is 28 × 4 × 13.3 ms ≈ **1.5 s** of
serialized work behind one block — against a 12-40 ms emulation. That is the number that decides
whether one classifier actor stays viable, and it is why `serialize_us` and per-round `classify_us`
have to be measured separately rather than folded into one counter. The escape hatch is unchanged
and already designed: N classifier actors sharded by `hash(trace_id)` (`em1-design.md` Q9), which
shards serialization along with classification. Note the 1.5 s figure compounds two worst cases
(every trace at the tail, every trace exhausting the round cap) and is a ceiling, not a forecast —
but the cap on it is set by the round cap, so it should be re-derived if that cap moves.

**A correctness constraint the rounds introduce** (S14). `classify_txs` appends into a
caller-owned result — `res.actions.push_back(std::move(act))` (`EmuClassifierActor.cpp:125`) — and
never clears it. That is correct today because it runs exactly once per request. Under the tier-2
fixpoint it runs once per round against the *same* `EmuClassifyResult`, so **`res.actions` (and
`res.unported_btypes`) must be cleared at the top of every round**, or each round's rows accumulate
on top of the previous round's and the payload ships duplicates. Belongs to whichever wave lands the
suspension loop; called out here because the serializer downstream cannot detect the duplication.

**One trap when reading the resulting numbers.** `classify_us` spans everything between
`started_us` (`EmuClassifierActor.cpp:181`) and `:293`, which already includes `write_capture()`
(`:216-224`) — a synchronous JSON write to disk. Serialization lands in exactly the same region. The
existing ~86-110 µs averages and the 13.3 ms max are clean baselines *because* every perf run so far
had capture off; they must never be compared against a capture-enabled run. Reporting `serialize_us`
separately (and keeping capture off in any run used for perf decisions) is what keeps the two costs
attributable.

**Alternative A — serialize inside `TraceInserter`.** Parallelises better (one actor per trace,
`TraceInserter.cpp:344-349`) and hides the cost behind Redis round-trips. Rejected: it puts
`mch::Action`, `Value` and the anchor into the insert TU, keeps the EM1.5 lifetime coupling alive,
and re-introduces exactly the objection that killed integration point (c) in `em1-design.md` §13 —
`TraceInserter::start_up` is a blocking body, so any fault there is a fault on the Redis path.

**Alternative B — serialize in the sink continuation** (`EmuShadow.cpp:142-154`). Runs on the
classifier's thread anyway, so it has the same cost as the recommendation with none of the
type-hygiene benefit: the result still has to carry `Action` + anchor across the promise. No reason
to prefer it.

**Alternative C — a dedicated serializer actor.** Over-engineering for a workload measured in tens of
µs; it also adds a hop and a second queue to reason about. Rejected.

---

## 4. Types, seam and MULTI integration

### 4.1 The payload type

New header `ton-index-worker/ton-mch-engine/src/emu/EmuActionPayload.h` — deliberately its own file
with only `<cstdint> <string> <utility> <vector>`, so `TraceInserter.h` can include it without
pulling `Value.h`/`ActionBuild.h`/`MchClassifierActor.h` into the emulator's insert TU:

```cpp
namespace mch {
// Everything the Redis MULTI needs, already serialized. Built on the classifier
// actor while the cell anchor is still alive (§3); no mch value types survive
// into the insert.
struct EmuActionPayload {
  // Bare msgpack (§2.8 / Q2: uncompressed — td::Gzip emits zlib framing, which
  // ton-index-go's magic sniff would miss). EMPTY == classification did not
  // produce a payload: the `actions` field is NOT written (§7).
  std::string actions_blob;
  // (account, "<trace_key>:<action_id>") for every (action, account) pair.
  std::vector<std::pair<std::string, std::string>> aai;
  // Accounts that appear in an action but in no transaction of the trace.
  std::vector<std::string> referenced_accounts;
  std::int64_t aai_score{0};        // Python's trace.start_lt == root tx lt
  std::size_t action_count{0};      // telemetry only
  const char *state{"unclassified"};// §7: written to the trace hash for observability
};
}  // namespace mch
```

`EmuClassifyResult` (`EmuTypes.h:85-106`) then loses `std::vector<Action> actions` and
`EmuCellAnchor anchor` and gains `EmuActionPayload payload` + `std::int64_t serialize_us`. The
`// DECLARED BEFORE actions` comment at `:88-96` goes away with them.

The classifier can build every one of these fields because `EmuTraceView` carries what Python reads
from the trace: `trace_id` **is** the Redis trace key (`EmuShadow.cpp:62` vs `TraceInserter.cpp:55`,
identical expression), the node addresses give the transaction-account set for the
`referenced_accounts` difference (`EmuTypes.h:54`), and the pre-order flatten makes `nodes[0]` the
root, whose parsed `lt` is Python's `trace.start_lt` (`indexer/indexer/events/blocks/utils/event_deserializer.py:149`).

Address formatting for the `_aai` keys and the `referenced_accounts` payload must be
`std::to_string(wc) + ":" + addr.to_hex()` — `td`'s hex is uppercase
(`ton-index-worker/external/ton/crypto/common/bitstring.cpp:542`), matching both
`TraceInserter.cpp:144` and Python's `AccountId.as_str()`.

### 4.2 The seam: widen `insert()`, keep the 3-arg sink

```cpp
// TraceInserter.h
class ITraceInsertManager : public td::actor::Actor {
 public:
  virtual void insert(Trace trace, mch::EmuActionPayload actions,
                      td::Promise<td::Unit> promise, MeasurementPtr measurement) = 0;
};
```

`TraceScheduler`'s ctor builds a 4-arg lambda and either decorates it or wraps it with an empty
payload, so **the sink type the four trace sources see does not change**
(`TraceScheduler.h:36`, `:116-125`):

```cpp
auto insert_with = [im = insert_manager_.get()](Trace t, mch::EmuActionPayload p,
                                                td::Promise<td::Unit> pr, MeasurementPtr m) {
  td::actor::send_closure(im, &ITraceInsertManager::insert, std::move(t), std::move(p),
                          std::move(pr), m);
};
if (mch_shadow_.prep) {
  mch_classifier_ = td::actor::create_actor<mch::EmuClassifierActor>("MchEmuClassifier", mch_shadow_);
  insert_trace_ = mch::decorate_sink(insert_with, mch_classifier_.get(), mch_shadow_.gate,
                                     mch_shadow_.nonblocking);
} else {
  insert_trace_ = [insert_with](Trace t, td::Promise<td::Unit> pr, MeasurementPtr m) {
    insert_with(std::move(t), {}, std::move(pr), m);
  };
}
```

`decorate_sink` therefore takes a 4-arg `inner` and returns a 3-arg sink — one signature change in
`EmuShadow.h`, no change at `TraceScheduler.cpp:285` or in the listeners. Every existing invariant
survives: the continuation still calls `inner` exactly once on all six outcome paths
(`em1-design.md` §16.5), passing an empty payload wherever classification did not run.

This settles **Q10** of `em1-design.md` §20 in favour of widening `insert()`. Attaching the payload
to `Trace` was the alternative; rejected because `Trace` is an upstream emulator type that already
took one field from us (`cell_anchor`, `TraceEmulator.h:142`), and because the payload is genuinely a
parameter of the insert, not a property of the trace.

### 4.3 Insertion point inside the MULTI

All additions go in one block placed immediately after the existing `root_node` /
`depth_limit_exceeded` appends (`TraceInserter.cpp:241-242`) and before the batched
`hset` (`:248-250`) — that is after both early returns (`:59-65`, `:111-116`), so a skipped trace
skips its actions too, which is the correct coupling:

```cpp
// EM2: the classifier's rows ride the SAME transaction as the trace they
// describe — a pending trace is never visible without them.
trace_fields_to_set.emplace_back("mch_classify_state", actions_.state);   // §7
if (!actions_.actions_blob.empty()) {
    trace_fields_to_set.emplace_back("actions", actions_.actions_blob);
    for (const auto &[account, member] : actions_.aai) {
        transaction_.zadd("_aai:" + account, member, actions_.aai_score);
    }
    for (const auto &account : actions_.referenced_accounts) {
        transaction_.publish("referenced_accounts", account + ";" + trace_key_);
    }
}
measurement_->set_otel_attribute("ton.actions.count",
                                 static_cast<std::int64_t>(actions_.action_count));
```

Ordering inside a MULTI is irrelevant to atomicity, so no constraint beyond "before `exec()`".

### 4.3a MANDATORY: the actions field needs its own finality guard

**The hazard.** Emissions of one trace can reach the MULTI out of finality order, and today the trace
data survives that only because `TraceInserter` guards every node individually: it loads the existing
node and *skips the subtree* when the stored node is more advanced than the one being written
(`TraceInserter.cpp:87-95`), with the empty-flatten short-circuit (`:111-116`) catching the case where
that leaves nothing. **The `actions` field has no equivalent.** A stale emulated emission landing
after a finalized one would overwrite finalized actions with emulated ones — silently, because
`HSET` has no ordering semantics — while the node blobs in the very same MULTI correctly refuse the
downgrade. The trace and its actions would then disagree about which emission they describe, which
is exactly the invariant EM2 exists to establish.

**Two independent races produce this, and per-trace ordering closes only one.** The classifier-side
race (a slower earlier emission answering after a faster later one) can be closed by serializing per
`trace_id`. The second cannot: each emission gets its **own** `TraceInserter` actor
(`TraceInserter.cpp:344-349`), and those actors contend for a shared connection pool whose waiters are
woken by a bare `std::condition_variable`
(`ton-index-worker/external/redis-plus-plus/src/sw/redis++/connection_pool.cpp:220-234`) — no FIFO,
no fairness guarantee. Two inserters for the same trace can therefore acquire connections and `exec()`
in an order unrelated to their creation order, no matter what the classifier did.

**Mechanism (mandatory, same wave as the tier-2 suspension loop).** Store the emission's finality
alongside the blob and refuse a downgrade, mirroring what the node loop already does:

```cpp
// Written with the blob; read back before overwriting it.
trace_fields_to_set.emplace_back("actions_finality", std::to_string(static_cast<int>(emission_finality)));
```

Before appending `actions`, read the stored `actions_finality` (the inserter already performs
`hget`s on this connection, `:291`/`:326`, so this adds one round-trip to a path that has several)
and skip both the `actions` HSET and the `_aai` ZADDs when the stored value is strictly more
advanced. Same comparison the node guard makes at `:92-93`, same direction, applied to the one field
that currently lacks it. `emission_finality` is the `min` over the view's node finalities — the same
value §2.3 already requires for the `finality` wire key, so nothing new has to be derived.

Wording to be kept in sync with `em2-tier2-design.md`, which carries the same finding from the other
side.

### 4.4 Re-emission semantics

- **`actions` field: whole-value overwrite, subject to the §4.3a finality guard.** `HSET` replaces
  the field; the blob is the complete action list for this emission. Recomputed per emission by
  construction — the classifier runs on every emission (`EmuClassifierActor.cpp:189-192` counts
  re-emissions but never skips them). Same as Python. No merge, no partial update: a trace whose tx
  set grew from emulated to finalized gets a wholly new list, which is the point — but a *stale*
  emission must not be allowed to write at all.
- **`_aai` ZADDs: additive, no cleanup.** Members are `<trace_key>:<action_id>`. If the action ids are
  unchanged between emissions, `ZADD` is idempotent (same member, score re-set). If they change,
  the old members linger. This is exactly Python's behaviour — nothing in the Python codebase, in
  `ton-index-go`, or in `ton-trace-ttl-tracker` removes `_aai` members on re-emission; removal
  happens only in the TTL sweep (`ton-trace-ttl-tracker/main.go:560-570`). And a stale member is
  benign: `FilterTraceActions` drops ids that are not in the decoded action list
  (`ton-index-go/index/crud/pending.go:69-81`), and `LoadRawTraces` silently skips trace keys whose
  hash is gone (`ton-index-go/index/emulated/repository.go:22-35`). **Reproduce as-is.**
  (An exact-cleanup variant — store the previous id list in a hash field, `ZREM` the difference at
  the next emission — costs one extra `HGET` on a path that already does several, and buys only
  garbage reduction. Proposed, not scheduled.)
- **`referenced_accounts`: republished every emission.** Python does the same; the subscriber merges
  into a set (`ton-trace-ttl-tracker/main.go:381`), so duplicates are free.
- **`aai_score`:** Python uses `trace.start_lt`, which does not change across emissions of one trace,
  so re-ZADDing the same member is a no-op in practice. (Redis scores are IEEE-754 doubles, so an
  `lt` above 2^53 loses precision — a pre-existing property of the protocol, identical for both
  writers.)

---

## 5. Admission work cap

**The gap.** `kClassifyDeadlineUs` is checked once, at dequeue, before any work
(`EmuClassifierActor.cpp:204-209`). Once a trace is admitted, nothing can shed it, and total added
insert latency is `queue_wait (≤200 ms) + classify_us + serialize_us` with no bound on the last two.
The measured tail is 13.3 ms for a single trace against a ~100 µs average — a 130× spread, i.e. the
cost is dominated by trace size, not by a constant. The perf work confirms the mechanism
empirically: the worst observed queue excursion (20.9 ms) was head-of-line blocking behind one slow
trace, not queue depth — the surrounding traces in that window classified at 23-29 µs. The deadline
could not have shed it, because it was already admitted.

**Why the emulator's own limits do not cover this.** `txs_count_limit_ = 256` / `trace_depth_limit_ =
32` (`ton-index-worker/ton-trace-emulator/src/EmulationContext.h:36-37`, enforced via
`is_limit_exceeded()` at `:82`) bound the **emulated** transaction count, and only that:
`increase_tx_count` has exactly **one** caller in the whole emulator,
`TraceEmulator.cpp:78`, inside the emulation of a single transaction, and `is_limit_exceeded()` is
consulted only at the emulation recursion gate (`TraceEmulator.cpp:86`, `:385`). The block emulators
never increment the counter for the committed transactions they read out of the block; they only
*report* the context's state onto the trace (`BlockEmulator.cpp:380`, `:671`). **So
`view.nodes.size()` is not bounded a priori by 256** — the committed portion of a finalized or
confirmed trace is ungated. Worth stating explicitly because the opposite ("the emulator refuses to
emit more than 256, so the input is bounded") is a natural and wrong reading of the same constants.

**Measured size distribution** (perf work, n=5,833 `--mch-capture` artifacts from the EM1 runs; one
`nodes` entry per transaction, so `len(nodes)` is exactly what the classifier sees). Sliced by node
finality, because the two paths are two different populations — and it is the *ungated* one this cap
exists for:

| population | n | p50 | p99 | max |
|---|---|---|---|---|
| purely finalized — block-sourced, **ungated** | 5,812 | 2 | 17 | **34** |
| mixed emulated+finalized — has an emulated tail, **gated at 256** | 21 | 17 | 57 | **57** |
| pooled | 5,833 | 2 | 17 | 57 |

Cost of a cap, pooled: 16 → 1.389% rejected, 32 → 0.137%, **64 → 0.000%**, 128 → 0.000%.

Two things follow. **The ungated path empirically stays far below the cap** — in 5,812 samples of
exactly the population with no 256 ceiling, the max was 34 txs, so 64 sits ~1.9× above the observed
max and ~3.8× above its p99. That is a weaker guarantee than an invariant, but it is evidence rather
than an assumption. And **the entire upper tail belongs to the gated path**: the seven largest traces
in the whole population (57, 57, 57, 57, 53, 53, 47) are all emulated-path, whose median is 17 txs
against 2 for pure-finalized. The distribution is not one smooth tail; the cap is really working
against the emulated population, which is consistent with the 13.3 ms trace being emulated-path at
the ceiling.

Caveats to carry, and they are sharper after the slicing: only **21** mixed traces in 5,833, from
finalized-focused runs with no overlay — so the emulated/pending population is barely sampled, and
it is precisely the one carrying the tail. Zero of the 5,833 had `tx_limit_exceeded` set, so none
reached the ceiling and this data cannot say what the emulated path looks like when it does.

**Cost per tx is bracketed, not known.** The 13.3 ms trace could not be identified (the journal
carries cumulative aggregates only), though it correlates strongly with a `tx_limit_exceeded` 0→1
step in the same 10 s window, which points at a 256-tx emulated trace. That gives 52 µs/tx at the
ceiling against 13-43 µs/tx for small traces — i.e. somewhere between roughly linear and modestly
superlinear. One data point with an unverified size: "linear with a ~2-4× penalty factor at the
ceiling" is the most that should be planned on, and no coefficient belongs in this design.

**Design.** A cap on `view.nodes.size()`, taken in the decorator immediately after `make_view`
(which already walked the tree, so the count is free) and *before* `in_flight` is incremented:

```cpp
// EmuTypes.h
inline constexpr std::size_t kMaxClassifyNodes = 64;    // --mch-work-cap
enum class EmuClassifyOutcome : std::uint8_t { …, shed_oversize };
```

That seam is the right one for a reason beyond convenience: the check happens before `in_flight` is
incremented and before the `send_closure`, so an oversized trace never consumes a queue slot and
never crosses a thread. Checking inside the actor next to the deadline test
(`EmuClassifierActor.cpp:204`) would also be before any parse work, but only after queueing.

Over the cap ⇒ `note_shed(shed_oversize)`, no request is sent, the insert proceeds immediately with
an empty payload. Counter `shed_oversize` joins the `shed{…}` group in the 10 s line
(`EmuClassifierActor.cpp:342-344`), and the first N over-cap traces log their node count at WARNING
so the cap can be calibrated from real traffic rather than guessed twice.

**On "the capped path emits basic-actions fallback instead of silence".** Considered and **not**
recommended as the default, for a reason worth stating: the basic fallback still needs the full
`ParseQuery::parse_transaction` pass over every node (`ClassifyCore.cpp:333-367` rebuilds the tree
from parsed transactions), which is the part that scales with N — so it saves the matcher pass and
little else, while producing an action list that contains *only* `ton_transfer`/`call_contract`/
`contract_deploy` rows for a trace that genuinely contains protocol actions. Because `Classified` in
Go is literally "the `actions` field exists" (`ton-index-go/index/emulated/models.go:838-844`), that
payload is reported to API consumers as a **successful** classification that is silently missing
every protocol action. "Not classified yet" is a state consumers already handle; "classified,
wrongly" is not. **DECIDED (Q3): cap ⇒ no `actions` field, `mch_classify_state = "shed_oversize"`,
loud counter, cap value 64.** This is also the one place §7's "every non-capped trace carries at
least basic actions" policy deliberately stops — hence its wording: over-cap traces are the
exception the policy names, not an oversight.

Calibration: **64** is chosen from the measured distribution above — ~1.9× the observed max of the
ungated path, and the smallest round value that rejected nothing in 5,833 real emissions. Because
the population under-samples exactly the traffic that carries the tail (21 mixed traces, no overlay
run), the flag `--mch-work-cap` must exist from day one so the value can move without a
rebuild, and `shed_oversize` must be watched during the first live runs. If it fires at all in
normal traffic, the cap is wrong, not the traffic.

**The measurement worth running first** (perf work's recommendation, and it is a good one): the
per-trace DEBUG line at `EmuClassifierActor.cpp:273-274` already carries `txs=` but no timing, and
every run so far capped at verbosity 3 so none were emitted. Adding `res.classify_us` to that line
and running a few minutes at `-v4` yields exact size/latency pairs across the whole distribution —
strictly better than extrapolating from one unidentified tail point. A second timestamp between
`emu_to_schema_txs` and `classify_txs` splits parse from matcher cost in the same pass.

**Log the finality alongside `txs` and `classify_us`, not just the count.** Given the bimodality
above, a max-observed `txs=` pooled across both paths hides the structure that matters: what is
needed is max and distribution *per path*, so the ungated path can be watched for any approach to 64
and the emulated path can be located against its own 256 ceiling. The value is already at hand at
that log site — `view.nodes.front().finality` is what feeds the `by_finality` counter
(`EmuClassifierActor.cpp:193-195`).

---

## 6. Basic-actions fallback: parity check

**Both sides have one; they differ in trigger coverage, not in output shape.**

C++: `basic_classify_fallback` (`ton-index-worker/ton-mch-engine/src/ClassifyCore.cpp:333-367`),
populated into `ClassifyResult::fallback_actions` (`ClassifyCore.h:59-71`) and triggered from the
`catch` around the matcher pipeline (`ClassifyCore.cpp:512-530`). It rebuilds the tree with
`init_block` and no matchers, runs the post-processor chain, and emits one row per surviving spine
block of a basic btype. The emu actor already selects it on failure —
`res.used_fallback = cres.failure; rows = cres.failure ? cres.fallback_actions : cres.actions`
(`EmuClassifierActor.cpp:113-116`) — and counts the rows separately (`stats_.fallback_actions`,
`EmuClassifierActor.h:29-32`). **So "always with actions" already holds for the failure path.**

Python: `try_classify_basic_actions` (`indexer/indexer/events/event_processing.py:397-416`), called
from `TraceProcessor.process_trace`'s exception handler (`indexer/indexer/events/trace_processor.py:60-79`).
Same construction (`to_tree` → `init_block` → post-processors → `serialize_blocks`), same reachable
btypes (`event_processing.py:145-156` only ever builds `TickTockBlock`/`TonTransferBlock`/
`CallContractBlock`, with `ContractDeploy` children).

**Two real gaps, both on the "unknown" side, neither on the fallback side:**

1. **No `create_unknown_action` in C++.** Python emits a synthetic `type="unknown"` row
   (`block_tree_serializer.py:1683-1706`) from two places: inside `try_classify_basic_actions` when
   the root has no children (`event_processing.py:399-400`), and from `try_classify_unknown_trace`
   (`:372-395`). Grep finds no `"unknown"` action type anywhere in `ton-mch-engine/src`.
2. **No zero-actions-without-exception branch.** Python calls `try_classify_unknown_trace` when
   classification *succeeded* but produced no actions (`trace_processor.py:44-46`). C++ never checks
   `result.actions.empty()` on the success path.

Consequence for EM2: a trace that Python would report as a single `unknown` action becomes, in C++,
a trace with an **empty** action list. That is still "has actions" as far as Go is concerned
(`Classified = has_actions` is field presence, not list length,
`ton-index-go/index/emulated/models.go:838-844`) — an empty msgpack array decodes fine and yields
zero actions with `classification_state: "ok"`. So it does not break; it silently loses the `unknown`
row that today tells a consumer "we looked and found nothing classifiable".

**REQUIRED, not optional — port both in W4.** Under the Q4 policy (§7: every non-capped trace carries
at least basic actions) a zero-action success must carry *something*, and today it carries an empty
array. `create_unknown_action` is ~15 lines against `mch::Action` (`type="unknown"`, `action_id` =
the trace key, the base lt/utime/tx_hashes/accounts fill `build_action` already computes), and the
empty-result check is one `if`. Together they make "never visible without actions" true in the strong
sense for every path except the two the policy explicitly reserves.

**LANDED (W4), with one correction to the sketch above.** `action_id` is the trace id, as designed —
but the lt/utime/mc_seqno_end range is **not** something `build_action` computes here, because there
is no Block to compute it from. Python reads all five off the Trace ROW (`:1692-1703`), and so does
the port.

That exposed a latent hole the moment the row existed: **`SchemaTraceLoader` never filled those
fields**, so `mch::Trace::{start_lt,end_lt,start_utime,end_utime,mc_seqno_end}` were zero on the
production path — and that is the loader BOTH classifier actors use (`EmuClassifierActor`,
`MchClassifierActor`). Every production `unknown` row would have shipped zero lt/utime/mc_seqno into
the write-back. It was invisible before W4 only because nothing read those fields on that path.
`--actions-schema` / `--actions-prod` caught it immediately (200 diff lines, all 50 unknown rows).

**Fixed in the loader, not at the call site** (lead's ruling): `fill_trace_aggregates`
(`TraceLoader.h`) derives the five as min/max over the transactions, and `schema_to_trace` calls it.
The fixture loader keeps reading the authoritative trace header. That leaves two independent sources
cross-checked by an existing gate — `--actions` (header-sourced) vs `--actions-schema`/`--actions-prod`
(derived) must stay byte-identical — rather than one source and an assumption. The equivalence also
holds directly: on all 165 corpus fixtures the header's five values *are* exactly that min/max, 0
disagreements. Fixing it at the `create_unknown_action` call site instead would have left the zeros
in place for the next reader of those fields.

**The W3 caveat is RESOLVED, and the answer is the reassuring one.** The worry was that an incomplete
emulated trace might carry a projected `end_lt` that `max(tx.lt)` cannot reproduce. It cannot,
because no such value exists on either side:

- the emulator's `::Trace` (`ton-trace-emulator/src/TraceEmulator.h:127-157`) carries **no**
  trace-level lt/utime/seqno at all — only hashes, the root node, `tx_limit_exceeded` and the cell
  anchor;
- **Python derives them per emission too**: `deserialize_event` builds the pending `Trace` with
  `start_lt = root_tx.lt`, `start_utime = root_tx.now`, `end_lt = max(tx.lt)`,
  `end_utime = max(tx.now)` (`indexer/events/blocks/utils/event_deserializer.py:146-151`).

So a pending trace's `end_lt` **is** `max(tx.lt)` on both sides, and the derived form is the correct
source rather than a stand-in. Two knowingly-accepted differences remain, neither reaching a
consumer: `start_lt` is `min(lt)` in C++ where Python takes the root tx's lt (identical for any
well-formed trace, the root being earliest), and `mc_seqno_end` is derived in C++ where Python leaves
it `None` — harmless, since Go has no field for it (§2.3).

---

## 7. Failure semantics — the degraded-mode answer

The end state says an emulated trace is never visible without its actions. Taken literally that would
mean *not inserting the trace* when classification does not produce a payload. **That is not
viable**, and the reason should be on the record: the same MULTI carries the account-state Lua CAS
(`TraceInserter.cpp:187-210`), the `new_finalized_txs`/`new_confirmed_txs`/`new_commited_txs`
publishes (`:213-223`), the `tr_in_msg:` reverse mapping (`:252-253`) and the `new_trace` publish
(`:258`) — all consumed by services that have nothing to do with actions — and the insert promise is
what releases the block emulator's completion chain (`em1-design.md` §18). Suppressing the insert
would turn a classifier hiccup into a blockchain-data outage.

**The policy (user, this round): every non-capped trace carries at least basic actions.** Concretely
that means the fallback rows are the *default* degraded output, and the empty degraded mode — no
`actions` field at all — is reserved for exactly two situations: an over-cap trace (§5), and a hard
failure where even the fallback is not computable. That second set is smaller than it sounds, and
worth naming precisely, because it is forced by construction rather than chosen:

- **`convert_failed`** — the fallback needs parsed transactions, and conversion is what produces
  them (`basic_classify_fallback` rebuilds from `ctx.trace`, `ClassifyCore.cpp:333-367`). Nothing
  exists to build rows from.
- **`shed_admission`, `shed_deadline`, `bypassed_disabled`** — these are *classifier-unavailable*
  states, not *classification-produced-nothing* states. Computing fallback rows requires the parse
  pass, which is precisely the work the valves exist to shed, dispatched to the actor that is
  already saturated or wedged. Honouring "at least basic actions" here would defeat the valve it is
  asking to bypass. They stay empty-degraded, and the practical cost is small: perf puts expected
  in-flight at ~60-250 against a cap of 1024, so all three should read zero in normal operation, and
  a non-zero count is itself the signal.

**This policy makes §6's port mandatory, not merely nice-to-have.** A classification that *succeeds*
with zero actions currently ships an empty array — which satisfies "the field exists" but not "the
trace carries at least basic actions". Python covers that case with `try_classify_unknown_trace`
(`trace_processor.py:44-46`) emitting a synthetic `unknown` row; C++ has neither the branch nor the
row. So W4 is now on the critical path for this policy rather than a parity nicety.

**Contract**, one row per outcome. `state` is written to the trace hash as
`mch_classify_state` (a new field; Go ignores unknown hash fields — `ConvertHSet` only reads
`root_node`, the node keys it walks to, and `actions`, `models.go:767-846`), so degradation is
visible in Redis without reading logs:

| outcome | `EmuClassifyOutcome` | `actions` field | `mch_classify_state` | Go sees |
|---|---|---|---|---|
| classified | `classified` | written (may be an empty array) | `ok` | `Classified: true` |
| classified, 0 rows | `classified` | written — **`unknown` row** (§6, W4) | `unknown` | `Classified: true` |
| classify failed | `classify_failed` | written — **fallback rows** (§6) | `fallback` | `Classified: true` |
| convert failed | `convert_failed` | **not** written | `convert_failed` | `Classified: false` |
| queue deadline | `shed_deadline` | not written | `shed_deadline` | `Classified: false` |
| admission cap | `shed_admission` | not written | `shed_admission` | `Classified: false` |
| work cap (§5) | `shed_oversize` | not written | `shed_oversize` | `Classified: false` |
| fail-open sticky | `bypassed_disabled` | not written | `disabled` | `Classified: false` |
| response lost | *(promise errored)* | not written | `response_lost` | `Classified: false` |
| nonblocking mode | *(not awaited)* | not written | `nonblocking` | `Classified: false` |
| feature off | — | not written | field absent | `Classified: false` |

The last three rows were added in W5, when the states stopped being hypothetical:

- **`response_lost`** — `~LambdaPromise` answers a promise the actor never got to (shutdown) with an
  error, so the sink's continuation still runs and the insert still happens. Suppressing the insert
  there would turn a classifier hiccup into a blockchain-data outage, which is the same reason the
  whole degraded-mode policy exists; the marker records that the trace went out unclassified.
- **`nonblocking`** — under `--mch-classify-nonblocking` the insert does not wait for the classifier,
  so no payload can be available to it by construction. That flag is a perf-measurement mode and
  never a write-back one; the marker makes a run that used it self-identifying in Redis rather than
  silently indistinguishable from a shed.
- **`feature off`** keeps `state == nullptr`, and the insert returns before the marker, before the
  otel attribute and before the guard's `hget` — so a build with no `--mch-artifact` issues exactly
  the command set it issued pre-EM2 and pays no extra round trip.

`Classified: false` is not a new state for consumers: it is exactly what every pending trace looks
like today between the emulator's insert and Python's classify. So the degraded modes degrade to
*today's normal*, which is the strongest honest guarantee available. Every one of them already has a
counter (`EmuClassifierActor.h:24-38`) and appears in the 10 s line; `shed_oversize` is the only new
one.

### 7.1 Tier-2 misses: DECIDED (Q4), split by kind

Not every tier-2 miss degrades the same way, and the resolution splits on exactly that. A stonfi v1
`dex_in` miss **rejects** the swap match — the action disappears, a clean coverage loss. A Tonco
deposit miss **silently falls back to TON** (`emu-integration-plan.md:120-121`; the tier-2 design's
§11 calls it "the nastiest of the set"), i.e. it emits an action carrying the **wrong asset**. Under
EM1.5 that row went to a log line; under EM2 it would go into the pending API.

**Decision (user), as originally drafted:**

- **Clean-reject kinds (stonfi-class): stay null**, exactly as `em2-tier2-design.md` §8 (D6)
  specifies. Classification proceeds, the action is simply absent, no `failure`, no fallback. Today's
  behaviour, unchanged.
- ~~**Wrong-value kinds (Tonco-class): degrade to basic-actions fallback rows**~~ — **STRUCK
  (lead, W5). Not implemented, and must not be.**

**Why the second bullet is dead.** It was written against the trace-scoped draft of the wrong-value
guard. The guard that actually landed is **action-scoped** (`em2-tier2-design.md` §8.3, lead's
ruling in its W1): a Tonco-class miss cleanly rejects *that action*, the rest of the trace classifies
normally, and a trace left with no actions at all already gets §6's synthetic `unknown` row. So there
is no state in which a whole-trace basic-actions fallback would be the right output — the two kinds
now degrade the same way, and the only difference between them is which one used to be capable of
publishing a wrong asset. `fallback_tier2` was that draft's `mch_classify_state` value and has been
removed from the §7 table with it; no code ever referenced it (verified: zero hits across `*.cpp`,
`*.h`, `*.py`).

The kind-level classification (which lookup kinds are wrong-value vs clean-reject) remains enumerated
on the tier-2 side; this doc consumes it and must not re-derive it. What survives from the original
reasoning is the *motivation* — a miss that publishes the wrong asset is worse than a miss that
publishes nothing — and the action-scoped reject satisfies it more precisely than a whole-trace
fallback would, without the collateral loss that made the draft uncomfortable: under the struck
bullet, a trace holding one correctly-classified stonfi swap and one wrong-value Tonco deposit lost
**both** to basic rows, because no `CoreAction` records which lookups its fills consumed and the
fallback is a whole-trace re-classify. Action scoping drops exactly the affected row.

---

## 8. A/B validation before the flip

Both writers on live traffic is the plan's suggestion, but a second emulator writing into prod Redis
is **not** safe as-is: it would duplicate the `new_trace` / `new_finalized_txs` / `new_commited_txs`
publishes (`TraceInserter.cpp:213-223`, `:255-258`) into live consumers. Two phases instead.

### Phase 1 — offline replay diff (no writes at all, run first)

1. Read-only sweep of prod Redis: `SCAN`/`HGETALL` on pending trace hashes, keeping `root_node`, the
   node blobs, the interface blobs and Python's `actions` field.
2. Feed the same trace through the mch replay path. EM1's capture format already exists for this
   (`--mch-capture`, `em1-design.md` §7), and the replay harness reads it back through the identical
   `emu_to_schema_txs` code. A small adapter that builds an `EmuTraceView` from a Redis HGETALL
   instead of a capture file closes the loop without touching the emulator.
   Caveat to state in the tooling: the Redis msgpack path loses extra currencies
   (`emu-integration-plan.md` §2.2), so replay-from-Redis is valid for the *action diff* but is not a
   substitute for the in-process converter.
3. Diff decoded maps (not bytes — §2.7), bucketed:
   - `protocol-not-in-mch` — the action type has no matcher in `ir/mch_ir.json` (§2.4 bucket C).
     Expected, not a mismatch.
   - `matcher-runs-fill-missing` — §2.4 bucket B. **This is the actionable bucket**; each entry is a
     `fill_*` to write.
   - `child-action` — Python row with a non-empty `ancestor_type` or a `parent_action_id`; mch emits
     no child actions (§2.5).
   - ~~`unknown-row`~~ — **RESOLVED in W4.** Both writers now emit Python's synthetic `unknown`
     row (§6); the 165-fixture gate shows 50 of them, byte-identical on both sides. No longer a
     bucket.
   - ~~`float-repr`~~ — **RESOLVED in W2.** Never reached the bucket list: the parity gate caught it
     as a `real-mismatch` and it was fixed on the C++ side by porting CPython's `repr(float)`
     (§2.7). Recorded here only so the absence of the bucket is not read as absence of the hazard.
   - `wire-type-divergence` — the four §2.6 keys. Expected by design.
   - `top-level-null` — Python's `_fill_*` assigns some top-level columns an explicit `None`
     (`action.asset = None` for a TON asset, and similar), which the fixed-schema `mch::Action`
     cannot distinguish from "never assigned" — both are a Null `Value`, and §2.1 sanctions omitting
     either. **Unobservable to the consumer**: every field it fires on is a pointer in Go
     (`models.go:530-601`), so absent and nil decode to the same nil. Lead-accepted rather than
     closed, because closing it needs a per-btype key table maintained in lockstep with every Python
     fill. 42 at the 165-fixture baseline.
   - `boc-framing` — a BOC-carrying field (`custom_payload` / `forward_payload`) whose bytes differ
     because `td`'s writer emits no CRC32C and pytoniq's does (`te6ccg…` vs `te6cck…`, 4 bytes).
     **Same cell**, verified by root hash on 97/97 corpus pairs. This is the §12 root-hash bar
     (`Value.h:111-115`) expressed on the msgpack side, and it is a permanent accepted divergence:
     the contract is the cell, not the framing. Note that the payload BYTES a consumer receives
     therefore change at the flip even though the decoded cell does not — if any consumer
     byte-compares BOC payloads, the §8 live phase is where that surfaces.
   - `tonco-first-asset-reject` — a Tonco deposit whose ROUTER jetton wallet does not resolve:
     Python publishes it as a **TON** deposit (`indexer/events/blocks/liquidity.py:1400-1405`,
     the "Jetton wallet not found for router_wallet_str" branch), mch drops the action instead
     (`HostTonco.cpp`, the §8.3 wrong-value guard of `em2-tier2-design.md`, landed in its W1).
     **Deliberate divergence, lead-confirmed**: the C++ behaviour is the intended end state and
     Python is retired in EM3, so this is counted, never "fixed" on the Python side. Same shape as
     `jvault_unstake_request` losing its non-early `assets == null` rows (that doc's Q-C).
   - `nft-content-null` — **a live production action loss, found by W7(iii)'s replay and not by any
     existing gate.** `ParsedBlockLookupSource::tier1` hard-codes the `nft_item` lookup's `content`
     field to Null (`ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:82-84`), because
     `NFTItemDataV2.content` is a flat `map<string,string>` and the fixtures' `content` is a nested
     metadata dict. `get_nft_data` then faults — `if (content == nullptr || content->t !=
     VType::Dict) return rt_fault("nft content is not a dict")`
     (`ton-index-worker/ton-mch-engine/src/host/HostNft.cpp:230-233`), mirroring Python's `"uri" in
     nft.content` raising on a non-dict — and the whole `nft_transfer` row is dropped. This is **not**
     a replay artifact: `emu/EmuInterfaces.cpp`'s `to_v2(NftItemDetectorR::Result)` drops `content`
     with the comment "not read by tier1", so an emulated trace in production loses the same rows
     today. The reason nobody has seen it is that `--actions-prod` is structurally blind to it:
     `SchemaGate.cpp:205-224` deliberately declines to synthesize the three nft kinds into V2 and
     routes them to a fixture-backed tier-2 fallback, so the offline prod gate never exercises tier-1
     nft_item at all. **4 rows on the 165-fixture corpus**, so the fix carries gate coverage the
     moment `content` is plumbed. Fix and scheduling live in `em2-tier2-design.md` §6.1a (a post-W4
     mini-wave, NOT folded into the in-flight suspension work). Note it is **not** closed by tier 2:
     the kvrocks decoder nulls `content` as well.
   - `real-mismatch` — everything else. Target: zero.
4. Also assert **decodability**: run the emitted blob through a Go decode harness against
   `emulated.Action` so a type slip is caught by the actual consumer's decoder, not by our reading of
   it.

**Phase-1 expectations, measured** (W7(iii) dry run, `mch-ab-replay` over the 165-fixture corpus
replayed through the emulator's own wire structs). The replay tool separates two things that would
otherwise be confounded, and both numbers are the baseline a prod sweep should be read against:

- With lookups served from the fixture corpus — the **converter** in isolation — the replay
  reproduces `ton-mch-engine --actions-msgpack` **byte for byte**, 277/277 rows, all three
  `ab_replay_diff.py` fatal buckets empty and the coverage buckets at the documented baseline
  (11 / 97 / 42 / 1662).
- With lookups served from the dump's own `AddressInterfaces` blobs — **what EM2 actually sees** —
  the corpus yields **272 rows, 14 lost**: 6 jvault (`jvault_assets` is tier-2-only), 4 dedust
  `jetton_swap` (`dedust_pool` has no variant in `AddressInterfaces`; confirmed against
  `--lookup-keys`), and the 4 `nft-content-null` rows above. Nine of the affected traces fall to zero
  actions and gain §6's synthetic `unknown` row, so the row count moves by 5, not 14 — a reminder
  that a shrinking `real-mismatch` count is not by itself evidence of coverage.

The first two buckets are the tier-2 gap and close when `em2-tier2-design.md` lands; the third is
independent of tier 2 and is a tier-1 defect.

### Phase 2 — shadow write into prod Redis (APPROVED, gated on phase 1 coming back clean)

**DECIDED (Q5): approved**, with the gate that the offline replay diff must be clean first — phase 2
is a confirmation on real in-process input, not the primary evidence. Separate keys, separate
prefix, EXISTS-guard, publish nothing, exactly as designed below.

Add two flags so the writer can be pointed at non-colliding keys:

- `--mch-actions-field <name>` (default `actions`) — phase 2 uses `actions_mch`
- `--mch-aai-prefix <prefix>` (default `_aai:`) — phase 2 uses `_aai_mch:`

Both are ignored by every consumer (`ConvertHSet` reads only the keys listed in §7; nothing reads
`_aai_mch:`), so a phase-2 run is inert for the API.

The shadow writer must **not** re-insert the trace or re-publish. Concretely, a
`--mch-actions-only` insert manager (a sibling of `NullTraceInsertManager`,
`TraceInserter.h:27-32`) that writes only the two extra keys, guarded by a small Lua so it can never
create an orphan hash if our emulator wins the race against prod's:

```lua
if redis.call('EXISTS', KEYS[1]) == 1 then redis.call('HSET', KEYS[1], ARGV[1], ARGV[2]) end
```

Then the comparator reads `actions` (Python) and `actions_mch` (ours) from the same hash and applies
the same bucketing as phase 1 — now over genuinely in-process converted input, including extra
currencies. `referenced_accounts` is **not** published in phase 2 (it would pollute the TTL
tracker's address sets); the `_aai_mch:` keys are left to be dropped manually after the run.

### Flip criteria

`real-mismatch = 0` over a full day of traffic; `matcher-runs-fill-missing` either empty or an
accepted, listed regression; `shed_*` counters at zero outside deliberate stress; `serialize_us` +
`classify_us` p99 inside the block-emulation budget (`em1-design.md` §18). The flip itself is a
one-line default change, and the rollback is the same line.

---

## 9. Wave plan

Each wave is compile-clean on its own and leaves the tree shippable. Gates are the existing set
(`mch-cpp-plan.md` §1) plus what the wave adds.

| wave | content | gate |
|---|---|---|
| **W1** | `EmuActionPayload.h`; `Value`→msgpack writer (§2.7) + the top-level int/str rule (§2.2) + the §2.6 divergence, as a pure library function `serialize_actions(const std::vector<Action>&, const EmuTraceView&) -> std::string` in `mch-emu` | builds; `mch-classify` / `ton-index-postgres` untouched |
| **W2** | **Parity twin.** New harness mode `--actions-msgpack` writing the blob per fixture, and a `twins/py_actions_msgpack.py` that emits `msgpack.packb(a.to_dict())` from the same real `block_to_action` the existing twin already calls (`ton-index-worker/ton-mch-engine/twins/py_actions.py:1-11,172-192`), plus a comparator that diffs **decoded maps** with the §8 buckets | 165 fixtures, `real-mismatch = 0`. This is the wave that actually proves the contract, and it runs offline with no emulator, no Redis, no node DB |
| **W3** | `EmuClassifyResult` carries `EmuActionPayload` + `serialize_us`; drop `anchor` and `std::vector<Action>` from the result (§3); payload assembly (`_aai` members, `referenced_accounts`, `aai_score`, `finality`, `trace_external_hash_norm`) in `EmuClassifierActor::classify`; stats line gains `serialize_us{avg,max}` — **DONE**. One serialization call site (`build_payload`) so the tier-2 suspension loop re-wraps that, not the writer. `mch_classify_state` set per the §7 table at each outcome; W4's `unknown` row wired on the emulated path too. Stats line also carries `actions_bytes` and `ser{float,cell,unrenderable}` — all three of which should stay zero forever | builds; twin gate from W2 still green (the writer is the same function) |
| **W4** | `create_unknown_action` port + the empty-result branch (§6) — **DONE**. `ClassifyResult::unknown_trace` is raised by the core when a SUCCESSFUL classify matched nothing over a non-empty trace (`ClassifyCore.cpp`, mirroring `trace_processor.py:44-46`); the adapter answers it with `create_unknown_action(ctx.trace)` (`ActionBuild.cpp`), which has no Block and so cannot be a `CoreAction`. **Re-baseline: 227 → 277 rows**, one `unknown` per zero-action fixture (50 of 165), and the `--actions` text twin still diffs to 0 — Python emits the same 50 rows | twin gate extended with a trace that classifies to zero actions |
| **W5** | Seam widening (§4.2) + MULTI integration (§4.3) + **the §4.3a `actions_finality` guard** + `mch_classify_state` + `ton.actions.count` otel attr; `--mch-actions-field` / `--mch-aai-prefix` / `--mch-actions-only` (§8 phase 2) | builds: `mch-classify mch-emu mch-emu-bridge ton-mch-engine ton-trace-emulator`; feature-off byte-neutrality re-reviewed as a code item (`em1-design.md` §19 item 4); a review item asserting the guard refuses a lower-finality overwrite |
| **W6** | Admission work cap (§5) + the `shed_oversize` outcome/counter; the bucket-B `fill_*` functions (§2.4 enumerates them statically from `ActionBuild.cpp:521-553` — **no dependency on the A/B**, which only ranks them by live frequency) | twin gate covers each new fill |
| **W7** | A/B tooling: the Redis→`EmuTraceView` replay adapter, the bucketing comparator, the Go decode harness (§8) | phase-1 run on prod Redis, read-only |
| **W8** | Flip: `--mch-actions-field` default stays `actions`, EM2 becomes the writer, Python `--emulated-traces` retirement is a separate op decision (EM3) | §8 flip criteria |

W1-W2 are the critical path and carry all the contract risk; W3-W5 are mechanical once the writer is
gated. W6 and W7 are independent of each other and can run in parallel — W6's fill list comes from
the static enumeration in §2.4, not from A/B output; the A/B only tells you which of them to write
first.

**Cross-doc sequencing.** Two items here are shared with `em2-tier2-design.md` and must land in the
same wave as its suspension loop, not before or after: the §4.3a finality guard (both docs found it
from opposite ends) and the §3 per-round `res.actions` clearing. Whichever doc's wave lands first
should carry both.

---

## 10. Decisions and remaining open items

**Resolved this round (user + lead).** All six are folded into the sections above; listed here so the
rationale stays findable.

| id | decision | where |
|---|---|---|
| Q1 | **Diverge**: emit the affected composite fields as msgpack integers; A/B carries a named `wire-type-divergence` bucket | §2.6, §8 |
| Q2 | **Uncompressed msgpack first**; real gzip framing only if the Redis memory delta justifies it | §2.8 |
| Q3 | **Over-cap ⇒ no `actions` field** + `mch_classify_state` marker; cap = 64 as calibrated | §5, §7 |
| Q4 | **Split by kind**: wrong-value misses (Tonco-class) ⇒ basic-actions fallback rows; clean-reject misses (stonfi-class) stay null. Empty-degraded reserved for over-cap and hard failures | §7.1 |
| Q5 | **Shadow writer on prod Redis approved**, gated on the offline replay diff coming back clean | §8 phase 2 |
| Q6 | **`_aai` additive, no cleanup**, as designed | §4.4 |

**Still open — none blocking implementation:**

- **The `unknown`-row shape.** §6's port makes W4 load-bearing for the Q4 policy (a zero-action
  success must carry *something*). Python's `create_unknown_action` sets `action_id = trace.trace_id`
  (`block_tree_serializer.py:1683-1706`); on the emulated path `trace_id` is nulled
  (`pendings.py:236-239`), so the C++ row should use the trace key. Worth one line of confirmation
  from whoever owns the consumer expectations, since it is the only action id not derived from a
  hash.
- **Fallback granularity under Q4.** Whole-trace fallback is the only variant implementable without
  new plumbing (§7.1). If the A/B shows it costing well-classified actions too often, the half-measure
  is a kind→btype suppression map. Revisit with A/B data, not before.
- **Python-side convergence for the other seven §2.6 fields.** Our divergence fixes the two mch emits;
  multisig, layerzero_send, dvn_verify and the two cocoon fields still drop whole traces through the
  Python writer. Separate ticket, outside EM2's scope, but it should exist.

---

## 11. Corrections to the existing docs

- **`emu-integration-plan.md:83`** — "A C++ writer must reproduce it byte-for-byte." Not achievable
  and not required. Python's msgpack map order is `__dict__` insertion order, which follows whatever
  order a `_fill_*` happened to assign attributes; the Go decoder is order-insensitive
  (`decode_map.go:327-353`). The bar is **decoded-map equality**, per key. §2.7.
- **`emu-integration-plan.md:79-80`** — the wire contract is cited as
  `ton-index-go/index/emulated/models.go:530-600`. The `Action` struct is at `:530-601` and the
  detail structs it points into are at `:202-528`; the decode + post-decode overwrite that determines
  which fields a writer may omit is at `:821-836`. Worth adding, since "which fields are clobbered"
  is not derivable from the struct alone.
- **`emu-integration-plan.md:170`** — "`Action.accounts` (mch produces accounts since Phase G step
  2)" is correct (`ActionBuild.cpp:597-621`), but the plan does not mention that the *other* consumer
  of that list is the `_aai` index, which is Python-only today. §4.1.
- **`emu-integration-plan.md:172-176`** — "VERIFY whether ClassifyCore emits Python's unmatched-trace
  fallback". Verified: it does (`ClassifyCore.cpp:333-367`, wired at `:512-530`, already selected by
  the emu actor at `EmuClassifierActor.cpp:113-116`). What is **missing** is Python's `unknown`
  action and the zero-actions-without-exception branch — a different gap than the one the plan
  anticipated. §6.
- **`em1-design.md` §15, `EmuTypes.h:88-96`** — the anchor-before-actions declaration-order rule and
  its "a caller that moves `actions` out … is back to a use-after-free" caveat are now **dead and
  deleted** (W3): `EmuClassifyResult` carries `EmuActionPayload` — plain strings — so neither the
  anchor nor any `Value` crosses the promise, and the ordering rule it depended on is gone with it.
  `em1-design.md` §15 should be updated to match.
- **`em1-design.md` Q10 (§20)** — answered: widen `ITraceInsertManager::insert`, keep the 3-arg sink
  by wrapping in the scheduler ctor. §4.2.
- **`mch-cpp-plan.md:307-308`** (§10 hazards, "Leaf `.data` in C++ mirrors a SUBSET — extra_currencies
  / ton_transfer comment omitted") — partially stale: `fill_ton_transfer` fills the comment today
  (`ActionBuild.cpp:509-517`). The `extra_currencies` half stands but is **irrelevant to the wire**,
  because Go has no field for it and hardcodes an empty map (`models.go:975`); it only matters for the
  Postgres path.
- **New, not in any doc** — `td::Gzip`'s encoder produces zlib framing while its decoder auto-detects
  both (`Gzip.cpp:46-50` vs `:61`). Any C++ code that gzips *for a non-td consumer* must not use it.
  §2.8.
- **New, not in any doc** — the `referenced_accounts` channel is an unauthenticated remote-kill
  surface for `ton-trace-ttl-tracker`: a payload without a `;` panics the subscriber goroutine
  (`main.go:378-381`), and with no `recover()` anywhere in the module and only `wg.Done()` deferred
  on that goroutine (`:341`), the panic takes the process down. Not an EM2 defect — EM2 just becomes
  the publisher — but it is a hardening item someone should own. §1.
- **New, not in any doc** — the `actions` hash field has no finality guard, while every trace node in
  the same MULTI does (`TraceInserter.cpp:87-95`). Two independent races can deliver emissions out of
  order, and the second (inserter actors contending for the connection pool,
  `connection_pool.cpp:220-234`) is not closed by any per-trace serialization. §4.3a; found
  independently from the tier-2 side.
