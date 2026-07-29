# EM2 — tier-2 lookup wiring (design)

> Status: DESIGN rev 2 + final calls (2026-07-29) — **ACKED, W1 dispatchable**. Scope =
> program plan `emu-integration-plan.md` §1.4b (the pre-EM2 blocker) + §3 EM2's "tier-2 lookup
> wiring" bullet. Baseline = `3444c22` on `merge-v13` (EM1.5 as built). Companions:
> `em1-design.md` (§§11-20 = the request/response classifier this extends — **rev 2 invalidates its
> Q7 rationale, see §12.7**), `em2-writeback-design.md` (must state the same ordering mechanism as
> §5.2), `mch-cpp-prod-design.md` §3 (where the two-tier split was defined).
>
> Every claim about current code carries a `file:line`, re-verified against the working tree.
> §12 lists where the program plan's and em1-design's assumptions turned out to be wrong.
> Verification for this phase is COMPILE + EXISTING GATES + the new unit gate (§10 W4);
> the live tier-2 verification plan is §11.

## 1. Decisions

| # | Question | Decision |
|---|---|---|
| D1 | Where does the "collect pass" live? | **It does not exist yet — build it.** Today's two-phase fixpoint is per *(matcher, block)* build invocation and calls `LookupSource::fetch` synchronously (§3.1). Add a **trace-level** fixpoint: classify → collect wanted keys → batch fetch → rebuild context → re-classify, cap 4 rounds, commit the last pass (§4). |
| D2 | Actor shape | `EmuClassifierActor::classify` becomes **suspendable**: per-trace state in an id-keyed map, one `send_closure` per round to a pool of `EmuLookupActor`s, resume in the response. Mandatory companions: an **alarm-driven deadline sweep** (§5.5), a **tier-2 circuit breaker** (§5.6), and a **separate suspended cap** (§5.7). |
| D3 | Data source | **kvrocks only** (lead-confirmed), over the Redis protocol, demand-driven `MGET` on `ton-index:v1:<table>:<raw-addr>:payload` — the exact keys the C++ interface writer already produces (`KvrocksState.cpp:339-344,368-370`). No PG backend. |
| D4 | `dedust_pool` | kvrocks table **`dex_pools`**, per address — *not* the Python `dedust_pools` context map (§6.3). Side effect: removes the classification reason for an emulator `DedustPoolDetector`. |
| D5 | jvault's 3 rounds | **Internal to the resolver, not classify-level rounds.** The engine issues one `lookup jvault_assets(addr)` and never sees the intermediate `data_boc` keys (§6.2). |
| D6 | Cache | **No cross-trace TTL cache** (user override). Dedup is per-trace-fixpoint + re-emission-scoped only (§7). A stale `jetton_wallet.balance` is a wrong value, not a miss. |
| D7 | Miss policy | **Split by consumer**; wrong-value-on-miss is **ACTION-SCOPED** (lead call, overruling the literal trace-scoped reading). Clean-reject consumers keep `miss = null`; the one wrong-value consumer becomes a clean reject too, so only that action is dropped and the rest of the trace survives (§8.3). `basic_classify_fallback` stays reserved for whole-trace classify failures. |
| D8 | Ordering | A **finality guard on the actions HSET**, mirroring `TraceInserter.cpp:88-95`, is the primary mechanism; classifier-side per-`trace_id` serialization is the secondary (§5.2). |

The load-bearing structural finding is unchanged: **there is no trace-level collect pass today**, so
"one batched round per collect pass" is a thing to *build*. §4 is the core of the design and §10 W1
must land first.

## 2. What is broken today, precisely

`ParsedBlockLookupSource` is constructed with **no tier-2 hook** on both production paths —
`emu/EmuClassifierActor.cpp:106` and `MchClassifierActor.cpp:67`, both commented "tier-2 hook unset".
So `fetch()` evaluates `tier2_ ? tier2_(kind, args) : Value::null()` with an empty `std::function`
(`ParsedBlockLookupSource.cpp:149`) and every lookup tier 1 cannot serve is charged to
`misses` / `misses_by_kind` (`:152-155`).

By kind: `nominator_pool` and `jvault_assets` are **tier-2-only** (`ParsedBlockLookupSource.h:13-17`)
and their matchers classify zero actions today; `dedust_pool` has a tier-1 branch (`:117-127`) but no
emulator detector, so it always misses there; the four interface kinds are covered only for accounts
that *participate in this trace*, so every counterparty account a spec looks up is a miss.

The 165-fixture gates are blind to all of it: `SchemaGate.cpp:265-269` installs a **fixture-backed**
tier-2 hook over the Python-gathered superset (already containing the selector-extracted accounts and
pre-derived jvault records). They prove the *shape* of the two-tier split and nothing about sourcing.

## 3. How the engine actually asks for lookups

### 3.1 The fetch is synchronous, per-build, deep in the call stack

```
classify_trace                                          ClassifyCore.cpp:420
  └─ per included matcher, BFS over the spine                :467-492
       └─ try_build(m, fn, cur, arena, kinds, src, pending)   :481
            └─ run_two_phase(...)                             :39-41
                 └─ round 0..7: fn(env) vs CollectingLookupTable
                      └─ per miss: fetched.emplace(key, src.fetch(kind, args))
                                                       BuildDriver.cpp:243-245
```

`src.fetch()` runs inline on the classifier's thread, once per missing key, per build invocation.
There is no trace-wide key set and no seam to intercept one. The `CollectingLookupTable` fixpoint
(`BuildRuntime.h:88-103`, driver `BuildDriver.cpp:235-254`) is scoped to a single matcher firing on a
single block — it exists for lookup-arg-of-lookup *within one build program*.

`needs_lookups` (`ClassifyCore.cpp:38`) short-circuits matchers with no lookup and no host fn
(`BuildDriver.cpp:227-231`), so most matchers never touch the source.

**Two consumer families, both of which matter for §8:** spec `lookup` nodes (only 5 sites, §8.2) and
**host fns reading `env.lookups->get` directly** — `HostDedust.cpp:96`, `HostDedustDeposit.cpp:290,319,537`,
`HostNft.cpp:212,386,400`, `HostStonfi.cpp:402-406,674,687,860`, `HostTonco.cpp:56`. That is why
`dedust_pool` appears in no matcher's `ref_lookups` (`BuildRuntime.cpp:220-225`).

### 3.2 The key format

Every registered lookup takes one `Str` argument (`ParsedBlockLookupSource.cpp:136`), a raw address
`"<wc>:<64 HEX>"`.

The kvrocks id is `address_key(addr) = convert::to_raw_address(addr)` (`KvrocksState.cpp:368-370`) =
`std::to_string(wc) + ":" + addr.to_hex()` (`tondb-scanner/src/convert-utils.cpp:123-125`), and
`to_hex` uses the **uppercase** table (`external/ton/crypto/common/bitstring.cpp:542`). Python
normalizes identically (`indexer/core/kvrocks.py:39-45`; `AccountId.as_str()` at
`indexer/events/blocks/utils/ton_utils.py:94-97`).

**But the engine's argument is not guaranteed uppercase.** Tier 1 is case-insensitive by accident —
it parses via `block::StdAddress::parse` (`ParsedBlockLookupSource.cpp:50`) — while tier 2 receives
the raw `args` verbatim (`:149`). The resolver must therefore run `normalize_raw_address`
(`Value.cpp:225-258`, which uppercases the hex at `:250-257`) **before** building the key, and treat
a `nullopt` return as an immediate null, mirroring tier 1's malformed-address clean miss (`:51-53`).
A case mismatch here is a silent 100 % miss, so W3's gate asserts it on real payloads.

### 3.3 kvrocks key layout and payload encoding

`kvrocks_payload_key(table, id) = "ton-index:v1:" + table + ":" + id + ":payload"`
(`KvrocksState.cpp:339-344`); the value is a JSON object from `td::JsonBuilder` (`:559-566`),
decodable with `td::json_decode` — **no new dependency**.

Field encodings (`:421-476`): addresses → raw `wc:HEX` string; `int256` → **decimal string**; hashes →
base64 string; bools → JSON bool; `data_boc` → base64 BOC string.

## 4. D1 — the trace-level collect pass

### 4.1 Shape

```
emu_to_schema_txs(view, gv)                     ← ONCE per emission (the expensive TLB parse)
fetched = {}                                    ← key -> Value, monotonically growing
for round in 0 .. kMaxTier2Rounds:
    wanted = {}
    ctx    = rebuild_context(trace_id, txs)     ← schema_to_trace + to_tree + init_block
    src    = ParsedBlockLookupSource(&ifaces, recording_hook(fetched, &wanted))
    result = classify_trace(ctx, ir, setup, src)
    if wanted.empty():   → COMMIT this result (stats from src.stats())
    if round == cap:     → COMMIT this result, count tier2_rounds_exhausted
    if past deadline:    → COMMIT this result, count tier2_deadline          (§5.4)
    if breaker open:     → COMMIT this result, count tier2_breaker_open      (§5.6)
    values = <batched fetch of `wanted`>         ← the ONLY suspension point
    fetched.merge(values)                        ← misses stored as explicit nulls
```

The recording hook is a lambda, so **`ParsedBlockLookupSource` is not modified at all**:

```cpp
auto hook = [&fetched, &wanted](const std::string &kind, const std::vector<Value> &args) {
  const std::string key = lookup_key(kind, args);          // BuildRuntime.h:79, canonical keyer
  auto it = fetched.find(key);
  if (it != fetched.end()) return it->second;               // resolved, or a known null
  wanted.emplace(key, std::make_pair(kind, args));
  return Value::null();                                     // exactly today's tier-2-unset behaviour
};
```

Round 0 with an empty `fetched` is byte-identical to today for any trace whose lookups all hit
tier 1: `wanted` stays empty and round 0 commits. **Traces needing nothing pay what they pay today.**

### 4.2 Why a re-classify and not a resume

`classify_trace` mutates its inputs — it merges consumed blocks into produced ones
(`ClassifyCore.cpp:46-50`), reparents in the post-process passes (`:252-300`) and grows the spine
(`unwind_deployments`, `:309-325`). A second call on the same `TraceContext` runs over a half-eaten
tree. The context must be rebuilt: `schema_to_trace` + `to_tree` + `init_block`
(`EmuClassifierActor.cpp:96-104`), all O(#tx ≤ 256). The TLB parse (`EmuTraceConvert.cpp:17`) is
hoisted out of the loop and paid once.

### 4.3 Determinism — audit, with two known gaps

Re-running must be a pure function of `(txs, interfaces, fetched)`.

Clean: `post_process_dedust_liquidity` keys its dedup map by an address *string*
(`ClassifyCore.cpp:171-176`) and uses `std::set<Block*> removed` for membership only (`:189-209`);
`unwind_deployments` uses `std::set<Block*> visited` for membership only (`:310-323`);
`BuildEnv.bodies` is read by key only (`BuildRuntime.h:166`); the B1 inventory sets are
membership-only (`ClassifyCore.cpp:436-463`).

**Gap 1 — must be fixed in W1.** `HostDedust.cpp:154` builds
`std::unordered_set<const Block*> include(consumed.begin(), consumed.end())` and `:159-163` iterates
**that set** to collect `notif_blocks`, then `:165-166` `stable_sort`s by `min_lt`. Insertion order
into the sort is therefore address-dependent, and `stable_sort` preserves it **for ties**. Two
`DedustSwapNotification` blocks carrying equal `min_lt` would order differently between two passes of
the same trace and set different `in_leg`/`out_leg`. Distinct messages normally carry distinct `lt`,
so a tie is improbable rather than impossible — but "improbable" is what makes a byte-identity gate
flake intermittently, which is the worst failure mode to debug. Fix: iterate the `consumed` vector
(already ordered) instead of the set; the set stays for membership. Three lines, scheduled in W1
ahead of the gate.

**Gap 2 — latent, noted not fixed.** `ClassifyCore.cpp:204` sorts `children_blocks` by **pointer
value**. It reaches output indirectly: `unwind_deployments` walks `children_blocks` (`:312-313`), so
the order in which promoted `contract_deploy` blocks land on the spine — and hence the order of
`basic_classify_fallback`'s rows (`:353-361`) — can vary. Reachable only on a trace that both merges
DeDust deposits and takes the fallback. Left alone this wave: fixing it means choosing a new stable
key (`min_lt`?) whose effect on the existing baselines has to be measured, which is a separate change
from tier-2. Recorded here so the next byte-identity flake has a suspect.

### 4.4 Termination and the cap

`fetched` only grows; each round adds ≥ 1 key or converges; the per-trace key space is finite. Hard
cap `kMaxTier2Rounds = 4`. Rounds > 2 are genuinely reachable: a tier-2 hit can unblock a matcher
that previously rejected, the produced btype folds into the anchor inventory
(`ClassifyCore.cpp:489-491`), and a later btype-anchored matcher runs its own lookups.

**On cap exhaustion: commit the last pass and count** (`tier2_rounds_exhausted`) — lead-confirmed.
This deliberately differs from the inner loop, which throws (`BuildDriver.cpp:252-254`). Throwing
would make a trace that classifies fine today classify *worse* the moment tier-2 is enabled.

## 5. D2 — actor integration

### 5.1 Suspension

```cpp
void classify(EmuTraceView view, td::Promise<EmuClassifyResult> promise);     // entry, sig unchanged
void on_lookup_batch(std::uint64_t id, std::map<std::string, Value> values);  // resume
void on_lookup_failed(std::uint64_t id, td::Status why);                      // resume, all-null
```

Per-trace state lives in `std::map<std::uint64_t, std::unique_ptr<PendingClassify>> pending_` keyed by
a monotonic id — **not** a pointer captured in a closure, so a resume for a state the sweep already
committed is a clean no-op. `PendingClassify` owns `{view, txs, fetched, round, res, started_us,
deadline_us, promise}`.

Between the `send_closure` and the resume, `classify` **returns**: the mailbox keeps draining. That
is the point — a network round must not become a head-of-line stall.

### 5.2 Ordering (D8)

Suspension breaks completion order for repeated emissions of one trace: emission *N* can finish after
*N+1* if *N* needed a fetch. Harmless for stats; **a correctness bug for EM2's write-back**, where
both emissions `HSET <key> actions` and an inversion overwrites Finalized rows with Emulated ones.

**Primary mechanism — a finality guard on the actions field**, mirroring the per-node guard the
inserter already applies: `TraceInserter.cpp:88-95` loads the existing node and
`if (existing_node->finality > redis_node.finality) continue;`. The actions write takes the same
shape: read the stored finality alongside the actions field, skip the write when the stored emission
is strictly more final. This is robust against *any* reordering, including across process restarts
and across the Python writer during the A/B window — which classifier-side sequencing cannot cover.
**`em2-writeback-design.md` must state this identically**; it owns the field layout.

**Secondary — per-`trace_id` serialization in the classifier**: an
`unordered_map<string, deque<PendingClassify>>`; a second emission of an in-flight trace queues behind
the first. ~15 lines. It keeps stats attribution ordered and avoids two passes of the same trace
competing for lookups. Lead-confirmed as mandatory in the suspension wave.

### 5.3 The lookup actor

`EmuLookupActor` owns **one `sw::redis::Redis`** and issues **blocking** `MGET`s in its actor body.

Async redis++ is not an option: the async sources are gated behind `REDIS_PLUS_PLUS_BUILD_ASYNC`
(`external/redis-plus-plus/CMakeLists.txt:43-51,91-101`), which is unset (`build/CMakeCache.txt`
carries only `REDIS_PLUS_PLUS_BUILD_ASYNC_TEST:BOOL=OFF`) and would pull in libuv. Blocking-in-an-actor
is also the emulator's own precedent: `RedisInsertManager::insert` spawns a `TraceInserter` per trace
doing synchronous `hlen`/`hget`/`exec` (`TraceInserter.cpp:343-349`, blocking at `:56,260,291,326`).

The scheduler is `td::actor::Scheduler scheduler({threads})`, default 7
(`ton-trace-emulator/src/main.cpp:24,186`), one node with 7 CPU threads
(`external/ton/tdactor/td/actor/common.h:109-116`); a plain `create_actor` places on the *current*
scheduler (`common.h:472`), so a blocking `MGET` occupies one of those 7. **Pool of
`kLookupActors = 4`**, round-robined. `ActorInfoCreator::Options::on_scheduler(SchedulerId)`
(`core/ActorInfoCreator.h:37`) is available if isolation later proves necessary; not done now.

**Connection options must be set explicitly — the defaults are a deadlock.**
`ConnectionPoolOptions` defaults to `size = 1` and `wait_timeout = 0`
(`external/redis-plus-plus/src/sw/redis++/connection_pool.h:34,37`), and `wait_timeout == 0` means
`_cv.wait(...)` with **no timeout** (`connection_pool.cpp:230-233`). A shared `Redis` across four
actors would park three of them on the pool condvar forever. Required:

- **one `Redis` object per lookup actor** (not shared), each with `pool_size ≥ 2`;
- `wait_timeout` bounded (`kTier2PoolWaitMs = 50`) so pool starvation throws instead of parking;
- `connect_timeout ≤ socket_timeout` — the `KvrocksConfig` default is **10 s** connect
  (`ton-kvrocks-state/src/KvrocksState.h:37`), and a down store would cost 10 s *inside a
  non-preemptible actor body*, per reconnect. Set `kTier2ConnectMs = 150`, `kTier2SocketMs = 150`.

**Do not copy `TraceInserter.h:18`** (`redis_(sw::redis::Redis(redis_dsn))`) — it constructs from a
bare DSN and inherits exactly these defaults.

### 5.4 The deadline changes meaning

Today the deadline is tested once, at dequeue (`EmuClassifierActor.cpp:204-209`), and em1-design
§16.2 states plainly that it bounds queue wait only. With tier-2 the total is
`queue wait + Σ(classify passes) + Σ(fetch round trips)`. Two changes:

- **check `deadline_us` before issuing each fetch round** (§4.1); past it ⇒ commit the current pass,
  count `tier2_deadline`;
- pair it with the bounded timeouts of §5.3.

**Worst-case added insert latency, stated once so §5 and §6.5 agree:**
`deadline (200 ms) + connect (150 ms) + 3 × socket_timeout (450 ms) + one classify pass` ≈ **~800 ms**,
because a jvault round costs up to three chained `MGET`s (§6.5), not one. Rev 1's "deadline + one
socket timeout" understated this by a factor of three. If ~800 ms is too long for the tail, the levers
are `kMaxTier2Rounds` and `kTier2SocketMs`, both flag-tunable (§9).

### 5.5 Deadline sweep — the suspended-trace timer

**A suspended trace whose lookup response never arrives wedges the pipeline permanently**: its insert
promise never resolves, so `in_progress_cnt_` never releases, `finish_block_if_done` never runs, and
`finalized_blocks_inflight_` stays pinned against `kMaxFinalizedBlocksInFlight = 2`
(`TraceScheduler.cpp:24`). em1-design §16.3 accepted "no per-request timer" on the explicit ground
that "the classifier does no I/O on this path (tier-2 is unwired)". **This design invalidates that
premise**, so Q7's deadline heap comes back — scoped, cheaply, to suspended traces only:

`alarm()` today only prints the stats line (`EmuClassifierActor.cpp:307-353`). It gains a sweep over
`pending_`: any state past `deadline_us + kSweepGraceUs` is **committed with its current pass's
result** (all outstanding keys treated as null) and its promise answered. A late response for a swept
id finds no state and is dropped. Sweep interval `kSweepIntervalSec = 1.0` — independent of the 10 s
stats interval, so the alarm re-arms on the shorter of the two.

This is what §5.1's "a state the sweep already committed" refers to; it is a hard requirement of this
design, not an optimization.

### 5.6 Tier-2 circuit breaker

**The existing fail-open watchdog cannot catch a dead store.** `admit()` only evaluates staleness once
`in_flight >= kMaxInFlightClassify` (1024) (`EmuShadow.cpp:37-52`), but the block emulators
self-throttle: `kMaxFinalizedBlocksInFlight = 2` and `kMaxConfirmedBlocksInFlight = 8`
(`TraceScheduler.cpp:24-25`) at ~28 traces/block ≈ **280** block-sourced traces in flight — roughly a
quarter of the cap. Listener traffic is ungated and adds to that, but nothing guarantees the sum
reaches 1024, so a store that stops answering can stall the pipelines indefinitely while the watchdog
never fires. Two fixes:

1. **A breaker owned by the resolver.** `kBreakerFailures = 5` consecutive fetch failures or timeouts
   ⇒ open for `kBreakerOpenSec = 30`; while open, `fetch_batch` returns all-null **immediately**
   without touching the socket, so every trace degrades to today's tier-2-less behaviour at zero
   latency. One trial request on expiry closes it on success. Counted as `tier2_breaker_open` /
   `tier2_breaker_trips`, logged at WARNING on each transition.
2. **Make the stall watchdog evaluate staleness regardless of `in_flight`.** Move the
   `now - last_response_us > kStallFailOpenUs` test out of the `in_flight >= cap` branch in
   `admit()`. With the breaker in place this is defence in depth, but it is two lines and it removes a
   class of "wedged but under the cap" states that the current structure cannot express.

### 5.7 Suspended-trace cap and gate accounting

`in_flight` now counts requests that may be waiting on IO. **Do not lean on the 1024 valve** for
memory: the binding population is block budgets (~280) plus ungated listener traffic, and each
*suspended* trace additionally retains its converted `schema::Transaction` vector — message bodies as
base64 BOC strings — plus the cell anchor, for the whole IO round. That is a materially larger
per-item footprint than a queued-but-not-started request.

So: a **separate `kMaxSuspended = 256`**. Over it, the classifier commits the current pass without
fetching (`tier2_shed_suspended`) rather than suspending. New stat `suspended_max` alongside
`queue_us_max`. The 1024 admission valve and the sticky fail-open are unchanged in role.

## 6. D3/D4/D5 — the lookup backend

### 6.1 Kind → source → Value

Target shapes are `ParsedBlockLookupSource`'s tier-1 shapes (the A/B parity contract) or, for the
tier-2-only kinds, `FixtureLookupSource`'s.

| kind | kvrocks table | payload fields read | Value produced |
|---|---|---|---|
| `jetton_wallet` | `jetton_wallets` | `balance`(dec str), `address`, `owner`, `jetton` | `Obj{balance,address,owner,jetton}` — `ParsedBlockLookupSource.cpp:62-68` |
| `nft_item` | `nft_items` | `address`, `init`, `index`, `collection_address`, `owner_address` | `Obj{…,content=null}` — `:72-83` |
| `nft_sale` | `getgems_nft_sales` | `marketplace_address`, `nft_address`, `full_price`, `nft_owner_address` | `Obj{…}` — `:88-96` (**one shape covers V1 and V4**, below) |
| `nft_auction` | `getgems_nft_auctions` | `mp_addr`, `nft_addr`, `last_bid`, `nft_owner` | `Obj{…}` — `:107-115` |
| `dedust_pool` | **`dex_pools`** | `asset_1`, `asset_2` (nullable) | `Obj{assets:[{is_ton,address}×2]}` — `:117-127` |
| `nominator_pool` | `latest_account_states` | `code_hash` == `mj7BS8CY9rRAZMMFIiyuooAPF92oXuaoGYpwle3hDc8=` | any non-null `Obj{address}` — result is only null-checked (`nominator.mch:34-35`) |
| `jvault_assets` | chained, §6.2 | — | `Obj{staking_pool,asset,jvault_asset}` — `BuildRuntime.cpp:364-370` |

**`destroyed` is per-table.** Python treats a `"destroyed": true` payload as absent
(`interface_repository.py:126-127`, applied at `:244,:304` and across the batch gather at
`:839,:845,:851,…`). The field exists on `jetton_wallets`, `nft_items`, `getgems_nft_sales`
(`KvrocksState.cpp:719`), `getgems_nft_auctions`, `dex_pools` (`:809`) and `nominator_pools` (`:857`)
— but **not** on `latest_account_states`, whose payload builder emits no such field (`:590-607`). So
the destroyed rule is a no-op for `nominator_pool` and for both jvault `data_boc` reads, exactly as in
Python (`interface_repository.py:868-870` applies no destroyed check to states).

**`int256` fields arrive as decimal strings**, so `Value::make_int` needs a `string_to_int256`, not a
JSON number read.

**`nominator_pool` sources `latest_account_states.code_hash`** — the Python-parity rule the lead set:
source what Python's kvrocks combined reader reads for that kind, which is
`interface_repository.py:868-870`. It also shares jvault's `MGET`. kvrocks does carry its own
detector-populated `nominator_pools` table (`KvrocksState.cpp:2896`), but its population is the
detector's, not Python's, and using it would silently move the A/B baseline.

**GetGems V4 — review point partially disputed, with evidence.** The concern "V4 is precisely tier-2
traffic (no emulator detector)" is right and worth stating. But "the kvrocks decoder needs both
shapes" is not: `GetGemsNftFixPriceSaleV4Data` rows are folded into `batch.getgems_nft_sales`
(`KvrocksState.cpp:2183-2184,2227`) and written to the **same `getgems_nft_sales` table with the same
`PreparedGetgemsSaleRow` payload** (`:2877-2878`, builder `:699-722`). One decoder shape serves both,
and V4 resolves under the ordinary `getgems_nft_sales` key. The two shapes exist only on the *tier-1*
side (`ParsedBlockLookupSource.cpp:88-105`), where the V2 variants differ. Stated explicitly so an
implementer does not go hunting for a V4 table that does not exist.

### 6.1a KNOWN TIER-1 GAP — `nft_item.content` is Null on both tiers, and it drops actions

Note the `content=null` in §6.1's `nft_item` row. It is not cosmetic and it is not a tier-2 question:
**every `nft_item` lookup on the production path yields a Null `content` today, and the consumer
faults on it, dropping the whole action.** Found by EM2 write-back W7(iii)'s replay adapter, which is
the first thing to run the production lookup source over a corpus without a fixture fallback
underneath it.

The chain, all four links verified:

1. `ParsedBlockLookupSource::tier1` hard-codes it — `f.emplace_back("content", Value::null())`
   (`ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp:82-84`), with the comment
   "never read by the nft_item consumers (dns reads collection_address only)". That comment is wrong:
   `dns` is not the only consumer.
2. `get_nft_data` **faults** on it: `if (content == nullptr || content->t != VType::Dict) return
   rt_fault("nft content is not a dict")`
   (`ton-index-worker/ton-mch-engine/src/host/HostNft.cpp:230-233`), faithfully mirroring Python's
   `"uri" in nft.content` raising on a non-dict. A fault rejects the match, so the `nft_transfer` row
   is not produced at all.
3. `emu/EmuInterfaces.cpp`'s `to_v2(NftItemDetectorR::Result)` also drops `content` ("not read by
   tier1"), so an **emulated** trace in production hits the same fault. This is a live action loss,
   not a replay artefact.
4. The kvrocks decoder nulls it too (`ton-mch-engine/src/KvrocksDecode.cpp:183`), so landing tier 2
   as designed does **not** close it.

**Why no existing gate sees it.** `SchemaGate.cpp:205-224` deliberately declines to synthesize the
three nft kinds into V2 and routes them to a fixture-backed tier-2 fallback, for the float-field
reason documented there. The side effect is that `--classify-prod` / `--actions-prod` never exercise
tier-1 `nft_item` at all, so the offline production gate is structurally blind to this. W7(iii)'s
replay is the first path with no fixture underneath, and it loses **4 `nft_transfer` rows** on the
165-fixture corpus.

**Fix sketch** (four touch points, one open question):

- `ParsedBlockLookupSource::tier1` — build a Dict from `NFTItemDataV2.content` instead of Null.
- `EmuInterfaces::to_v2(NftItemDetectorR::Result)` — carry `content` through.
- `KvrocksDecode.cpp` — the payload already has it: `PreparedNftItemRow.content` is an
  `optional<std::string>` holding `content_to_json_string(...)`
  (`ton-kvrocks-state/src/KvrocksState.cpp:1096`), so the decoder JSON-parses it into a Dict rather
  than nulling it.
- `ton-mch-engine/src/abreplay/RedisReplay.cpp` — carry `NftItemSchema.content` (the emulator's Redis
  blob already stores it, `Serializer.hpp:618`), which today mirrors `EmuInterfaces` and drops it.

*Open question to settle before writing it:* `HostNft.cpp:230-263` uses `content` two ways — a flat
`uri` key lookup for the fragment.com special case, and a whole-dict passthrough as `metadata`
otherwise. `NFTItemDataV2.content` is a **flat** `map<string,string>` while the fixtures' content is a
nested metadata dict, so the `uri` branch is satisfied by the flat map but the metadata branch may
differ in shape from Python's. Resolve that against Python's `interface_repository.py` before
choosing the Dict construction, or the fix trades a dropped row for a wrong one.

**Coverage is already there**: the 4 corpus traces exercise this the moment `content` is plumbed, so
the fix ships with a gate rather than needing one built. Scheduling: §10's post-W4 mini-wave, **not**
folded into W4's in-flight suspension work.

### 6.2 jvault — the rounds are inside the resolver

The spec issues **one** lookup: `lookup jvault_assets(...)`
(`indexer/events/mch/specs/jvault.mch:103`, again `:160`). There is no `data_boc` lookup kind
(`BuildRuntime.cpp:226-228`), so the intermediate keys are invisible to the engine and can never
appear in a classify-level collect round. The chain lives in the resolver, mirroring what
`FixtureLookupSource` does against the fixture map (`BuildRuntime.cpp:310-392`).

Ground truth `extract_jvault_assets` (`indexer/events/blocks/jvault.py:36-69`):

1. `latest_account_states[stake_wallet].data_boc` → `load_address` ×2 → `(staking_pool, minter)`
   *(already in C++ against the fixture: `BuildRuntime.cpp:319-360`)*.
2. `latest_account_states[staking_pool].data_boc` → **parse** `skip_bits(1+32)`, `load_address`
   (admin), `load_address` (creator), `load_address` = **`lock_wallet_address`**
   *(Python: `derive_accounts_from_stake_pool`, `address_selectors.py:128-139`; the C++ fixture source
   consumes it pre-derived at `BuildRuntime.cpp:377-380`)*.
3. `jetton_wallets[lock_wallet_address].jetton` → the asset.

Partial results are semantic, not errors: pool resolved but its `data_boc` absent ⇒
`{staking_pool, asset=null, jvault_asset}` (`jvault.py:58-59`, C++ `BuildRuntime.cpp:376-378`);
anything later failing ⇒ whole result null.

Step 2 requires reimplementing ~10 lines *of* `address_selectors.py` inside the resolver — see §12.4
for why that does not contradict "do not port address_selectors".

### 6.3 dedust_pool — where the data actually lives

The plan points at "the shared `dedust_pools` map (`interface_repository.py:293-305`)". That map is
only the first branch of `get_dedust_pool`; the branch below (`:300-306`) reads the per-address
kvrocks `dex_pools` payload. Following the map means following a Python cache of a Redis blob:
`context.dedust_pools` is process-local, loaded from Redis key `I_dedust_pools:data`
(`dedust_pools.py:17,223-227`), which a **Python** sidecar writes (`dedust_pools.py:40-77`:
`SELECT … FROM dex_pools WHERE dex='dedust'` → `SET` → `PUBLISH dedust_pools_updates`).

**Decision: `dex_pools` per address**, populated by the C++ interface writer
(`KvrocksState.cpp:2903`, payload `:795-811`). Demand-driven, same batched `MGET`, no refresh
machinery, no Python. Side effect for the lead: this removes the *classification* reason to add a
`DedustPoolDetector` to the emulator (plan §2.4 / EM3).

### 6.4 Why kvrocks only

Lead-confirmed. Production runs kvrocks; the C++ client exists with pool, timeouts and Sentinel
support (`ton-kvrocks-state/src/KvrocksState.h:20-62`; second copy `ton-index-postgres/src/KvrocksClient.h:13-57`);
the emulator already links `redis++_static` (`ton-trace-emulator/CMakeLists.txt:44`). A PG backend
would add a `libpqxx` link (present at `external/libpqxx`, root `CMakeLists.txt:32`, but not linked by
`tondb-scanner` and hence not by `ton-trace-emulator-core`), a DSN flag, and a second copy of the
seven mappings. It is also unnecessary for testing (§9.1).

### 6.5 Batching contract

`EmuLookupActor::fetch_batch(std::vector<Key>, Promise<std::map<std::string, Value>>)`:

1. **As built: ONE `MGET` for the whole round, across tables** (accepted deviation from this
   sketch's "one pipelined `MGET` per table"). Every key is already a full payload key
   `ton-index:v1:<table>:<id>:payload`, so the table never has to be a separate command — same
   result, fewer round trips than a per-table pipeline. `nominator_pool` and jvault-round-1 ride
   this same fetch.
2. Only if jvault keys resolved a pool: a second `MGET` on `latest_account_states` for the pools.
3. Only if step 2 yielded a `lock_wallet_address`: a third `MGET` on `jetton_wallets`.

**Worst case 3 chained round trips per classify-level round**; the common case is 1. This is the
number §5.4's latency budget uses. Every requested key comes back present — **a miss is an explicit
null** — so the recording hook distinguishes "not fetched" from "fetched, absent" and the fixpoint
cannot loop on a permanently-missing key.

## 7. D6 — caching

**No cross-trace TTL cache** (user override, and the right call): the engine reads
`jetton_wallet.balance` into emitted fills, so a stale cache entry is a **wrong value**, not a miss —
the failure class §8.2 exists to eliminate. And a stale value is worse than the Tonco case §8.3
handles, because it is undetectable: §8.3 works precisely because the wrong value is produced from a
*known* null, which the code can branch on. A plausible-but-stale balance arrives indistinguishable
from a fresh one, so no guard can catch it. Serving one is strictly worse than serving null.

What remains, both free of staleness risk:

- **Per-trace fixpoint dedup** — the `fetched` map spans all rounds of one emission, so a key is
  fetched once per emission no matter how many rounds or how many matchers ask for it.
- **Re-emission-scoped dedup** — the 2-3 emissions of one trace (plan §3) reuse the `fetched` map
  carried on the per-`trace_id` queue entry (§5.2), bounded by the trace's own lifetime. This is what
  keeps re-emissions from paying IO three times, and it cannot outlive the trace.

**Deliberately deferred** as an optimization, to be revisited only with measured IO cost in hand: a
TTL cache. If it is ever added, balance-carrying kinds (`jetton_wallet`) are **excluded or given a
very short TTL**, and the decision needs the same user sign-off this one got. Also deferred:
invalidation on `new_account_state` (the emulator already publishes it, `TraceInserter.cpp:193`),
which would be the principled way to make caching safe.

## 8. D7 — failure semantics and the wrong-value carve-out

### 8.1 Baseline: infrastructure problems degrade, never fail

Connection refused, timeout, malformed JSON, unparseable address in a payload, breaker open, per-round
deadline, sweep, round-cap exhaustion, suspended-cap shed — **all produce nulls for the affected keys
and let classification and the insert proceed**. None sets `failure`, none maps to a
`FailureCategory`, none reaches the fallback. The em1-design §16.5 gate contract is unchanged and now
has more paths to hold across: one answer per request, `in_flight` decremented in exactly one place,
`inner(...)` called exactly once. Lookup promises are answered on lookup-actor death too
(`td::PromiseCreator` lambdas answer with an error on destruction) and `on_lookup_failed` is identical
to an all-null response.

### 8.2 The carve-out: consumers where a miss produces a *wrong* value

A null is safe only where the consumer rejects on it. Enumerated across both consumer families of
§3.1:

**WRONG-VALUE on miss — must become a clean reject (§8.3):**

| consumer | site | behaviour on a null lookup |
|---|---|---|
| `tonco_deposit_liquidity_data` | `HostTonco.cpp:337-341` | `jetton_wallet` miss ⇒ `first_asset = Value::make_asset_ton()`. The code comment states it: *"first asset (router wallet): null lookup -> fallback TON (NOT reject)"*. A missing counterparty wallet is published as a **TON deposit**. |

That is the **only confirmed member today** — across both consumer families, this is the single site
in the engine that answers a miss with a plausible wrong value rather than with nothing. It is
exactly the case the plan flagged, and the whole reason this carve-out exists.

**Degraded-but-not-wrong — a field goes null/absent; `miss = null` stays correct:**
`tonco_swap_data` leaves `destination_asset`/`min_out_amount` unset (`HostTonco.cpp:544-547,657-670`);
getgems purchase leaves `have_pd` false so the purchase block is absent (`HostNft.cpp:386-412`);
`get_nft_data` marks `exists=false` (`HostNft.cpp:205-225`); dedust deposit legs return a null record
(`HostDedustDeposit.cpp:290-296`); `dns` leaves the optional `collection_address?` null
(`dns.mch:22,31,37`).

**CLEAN REJECT on miss — current semantics already correct:**
`stonfi_v1_swap_data` (`HostStonfi.cpp:409` `rt_fault("stonfi_v1: dex_in wallet lookup miss")`);
`dedust_seq_destination` (`HostDedust.cpp:97-99` `rt_fault("dedust_pool missing for …")`);
`dedust_withdraw` (`dedust_withdraw.mch:38-39` `reject when lp_wallet == null`);
`nominator_pool_withdraw` (`nominator.mch:34-35`);
`jvault_unstake` (`jvault.mch:103-105`) and `jvault_unstake_request` in its `early` branch
(`jvault.mch:160-162`).

**One gap, now a W1 task (Q-C, approved):** `jvault.mch:160-172`'s *non-early* branch is not
reject-gated on `assets` yet still emits `asset: assets.asset` / `jvault_asset: assets.jvault_asset`.
Whether a null `assets` faults (⇒ clean reject) or yields null (⇒ degraded field) depends on
`rt_access` semantics for a null receiver — rather than leave that to chance, W1 adds the explicit
`reject when` gate and runs the same fixture check as the Tonco change (§10 W1 item 3).

### 8.3 Mechanism — action-scoped clean reject (lead call)

**Decided: action-scoped.** The intent behind the policy is two rules at once — *never publish a
wrong value* and *every trace still gets actions* — and a clean reject satisfies both, while the
trace-scoped reading satisfies the first by sacrificing the second for every unrelated action in the
same trace. So the Tonco deposit is dropped; the correctly-classified jetton transfer next to it
survives; the subtree falls through to lower-priority matchers and, failing those, to unknown rows —
exactly the stonfi-class behaviour that is already correct today.

**No new mechanism is needed. The implementation is one branch.** `tonco_deposit_liquidity_data`
already returns a **null record** for its reject cases — `HostTonco.cpp:326-328` and `:332-334` both
`return rt_ok(Value::null())` under comments reading "raises … -> reject" — and the spec gates on it:
`tonco_liquidity.mch:59-60` is `let r = tonco_deposit_liquidity_data(consumed)` followed by
`reject when r == null`. So the fix is to replace

```cpp
if (first_asset.is_null()) { first_asset = Value::make_asset_ton(); }   // HostTonco.cpp:339-341
```

with the same early return its two siblings use — `return rt_ok(Value::null())` — and the existing
`reject when r == null` turns it into a clean matcher rejection. No `rt_fault`, no exception type, no
throw to the classify boundary, no change to `BuildDriver`/`ClassifyCore`.

**Counter placement — routed around `FailureCategory`, deliberately.** The lead's note suggested
counting this as `FailureCategory::tier2_unresolved`, but that field cannot hold it:
`ClassifyCore.h:23-36` documents the enum as **"TRACE-TERMINAL reasons only"** and then names the
exclusions explicitly — *"Deliberately NOT enumerators: … clean_reject / parse_null / the per-lookup
tier misses (per-(block, matcher, lookup) events on the hot path — counted in
`ParsedBlockLookupSource::LookupStats`, or left uncounted). None of those can be the single
trace-terminal value this field holds, so an enumerator for them would be a state the type never
reaches."* An action-scoped reject is exactly a `clean_reject`: the trace succeeds and
`failure_category` stays `none`. Adding the enumerator would put a value into the field that no trace
can ever carry.

So the counter lives where that comment says it belongs — as a per-event stat:
`EmuTier2Stats::wrong_value_guard` (§8.4), incremented at the reject site, with the kind and the
matcher name in the debug log. Same telemetry, no contradiction with the taxonomy. The
`FailureCategory` addition proposed in rev 2 is **withdrawn**.

**One consequence still stands (Q-B, approved as a deliberate re-baseline):** the guard fires with
tier-2 *disabled* too, since the TON fallback triggers on any null. That is arguably a fix of today's
behaviour, but it can move a gate baseline — so **W1 checks whether any of the 165 fixtures takes
`HostTonco.cpp:339-341`**, and if one does, the resulting diff is documented in the wave report
rather than silently absorbed.

### 8.4 Stats

`LookupStats` (`ParsedBlockLookupSource.h:60-68`) keeps `tier1_hits`/`tier2_hits`/`misses`/
`misses_by_kind`. A sibling struct on the actor holds the rest, so `ParsedBlockLookupSource` stays
untouched:

```cpp
struct EmuTier2Stats {
  std::size_t requested{0}, served{0}, absent{0};      // asked / non-null / null
  std::size_t fetch_errors{0}, breaker_trips{0}, breaker_open{0};
  std::size_t rounds{0}, rounds_exhausted{0}, deadline_hits{0}, swept{0}, shed_suspended{0};
  std::size_t wrong_value_guard{0};                    // §8.3 action-scoped rejects (NOT a FailureCategory)
  std::size_t fetch_us_total{0}, fetch_us_max{0}, batches{0}, suspended_max{0};
  std::map<std::string, std::size_t> requested_by_kind;
};
```

Only the **final** pass's `src.stats()` folds into the cumulative `LookupStats` — each pass builds a
fresh source, so intermediate passes must not double-count. The `[mch-emu]` 10 s line
(`EmuClassifierActor.cpp:322-350`) gains one `tier2{…}` group.

## 9. Config surface and the test run on this box

Following the `--mch-*` convention (`ton-trace-emulator/src/main.cpp:88-126`) and the kvrocks flag
names of `ton-index-postgres` (`ton-index-postgres/src/main.cpp:122-164`):

```
--mch-kvrocks <uri>                tcp://host:port/db — enables tier-2. Requires --mch-artifact.
--mch-kvrocks-sentinels <list>     host:port,…  (mutually exclusive with --mch-kvrocks)
--mch-kvrocks-master <name>        required with --mch-kvrocks-sentinels
--mch-kvrocks-user / -password / -db
--mch-tier2-connect-ms <n>         default 150   (NOT the KvrocksConfig 10s default — §5.3)
--mch-tier2-socket-ms <n>          default 150
--mch-tier2-rounds <n>             fixpoint cap, default 4                        (TEST ONLY)
--mch-tier2-max-suspended <n>      default 256                                    (TEST ONLY)
--mch-tier2-breaker <fails>:<sec>  default 5:30                                   (TEST ONLY)
```

No cache flags — there is no cache (§7). Absent `--mch-kvrocks*` ⇒ no lookup actors, the hook returns
all-null, the fixpoint converges at round 0 ⇒ **byte-identical to today**. Startup validation mirrors
`ton-index-postgres/src/main.cpp:521-527` (sentinels XOR uri; master required with sentinels) and logs
one `[mch-emu] tier-2 ENABLED (…)` line with the **host only**.

### 9.1 What a test run on this box looks like

Environment, discovered read-only:

- **No kvrocks on this box.** No unit on disk, no listener (`ss -lntp` shows only `:6379`), no
  container. The only Redis-protocol store is `redis-server.service` on `localhost:6379`.
- `/etc/systemd/system/ton-event-classifier-emulated.service` runs
  `event_classifier.py --emulated-traces --use-combined-repository --batch-time-window=0.2` and sets
  **only** `TON_INDEXER_PG_DSN` (`…/indexer_3`) and `TON_INDEXER_REDIS_DSN` (`localhost:6379`) — no
  `TON_INDEXER_KVROCKS*`. Since `kvrocks.is_enabled()` is `bool(settings.kvrocks or
  settings.kvrocks_sentinels)` (`indexer/core/kvrocks.py:35-36`) and both default to `''`
  (`indexer/core/settings.py:20-21`), `use_kvrocks` is **false** here (`pendings.py:332`) and combined
  mode resolves to the **PG** gather (`pendings.py:308-313` → `interface_repository.py:982`).
  Credentials: from that unit file, never copied anywhere.
- `/etc/systemd/system/ton-trace-emulator.service` points `--redis` at the **same** `localhost:6379`.

So the local test is: **seed plain Redis with real payloads and point `--mch-kvrocks` at it.**

1. A dev-utils script reads the accounts a capture wants (from the `[mch-emu]` miss log or a
   `--mch-capture` dump), `SELECT`s the matching rows out of `indexer_3` (read-only), and `SET`s
   `ton-index:v1:<table>:<RAW_ADDR>:payload` — **generating the JSON with `KvrocksState`'s own payload
   builders**, not hand-rolled from this doc's table.
2. `systemd-run -p LimitNOFILE=99999 -p RuntimeMaxSec=600 ton-trace-emulator … --mch-artifact …
   --mch-classify-only --mch-kvrocks tcp://localhost:6379/0`. `--mch-classify-only` means **zero**
   Redis writes to production pending data (`ton-trace-emulator/src/main.cpp:96-99`), so the run
   cannot touch the live pending pipeline.
3. Compare the `[mch-emu]` `lookups{…}` line against a run without `--mch-kvrocks`.

kvrocks is a Redis-protocol server and the backend only issues `MGET`, so this exercises the real code
path; and both sides then derive from the same `indexer_3` rows, making the Python A/B consistent by
construction. A run against a live kvrocks over VPN is a better *data* test and should follow, but it
is not a prerequisite.

## 10. Wave plan

**W1 — trace-level iterative driver, no IO.** Add `ClassifyDriver` (new TU in `mch-classify`)
implementing §4 with a *synchronous* `Tier2Fetch` callback; rewire `EmuClassifierActor::classify_txs`
(`EmuClassifierActor.cpp:84-138`) and `MchClassifierActor.cpp:55-78` through it with a null callback.
**Three more items in this wave**, each with its own fixture check because each can move a baseline:

1. the §4.3 Gap 1 fix in `HostDedust.cpp:154-166` (iterate `consumed`, not the set) — it must precede
   the byte-identity gate or the gate flakes intermittently;
2. the §8.3 one-branch change at `HostTonco.cpp:339-341` (`return rt_ok(Value::null())`), plus the
   corpus check: does any fixture take that branch?
3. the Q-C gate on `jvault.mch:160-172`'s non-early branch — add the missing `reject when` so a null
   `assets` cannot reach `asset: assets.asset`, plus the same corpus check.

If (2) or (3) moves a dump, that is a **deliberate re-baseline documented in the wave report** — never
absorbed silently.

*Gate:* the 165 fixtures via `--classify-prod` / `--actions-prod` produce **byte-identical** dumps to
the pre-wave binary (modulo any re-baseline from (2)/(3), reviewed and recorded), with the fixture
tier-2 hook in place, forced through the driver. That is the determinism assertion §4.3 needs. Rev 1's
second variant ("always-null tier-2 vs pre-wave binary") is **dropped** (lead-confirmed):
`prod_lookup_factory` unconditionally constructs `GateProdLookupSource` with the fixture hook
(`SchemaGate.cpp:260-278`), so it would need a new always-null mode in both binaries — more gate
surface than the property is worth, and W2's key-set dump exercises the round-0-commit path anyway.

**W2 — recording hook + the key-set gate.** The recording hook, `lookup_key`-based `wanted` set,
`EmuTier2Stats`, and a harness mode dumping the collect-pass key set per fixture
(`--lookup-keys`, `src/LookupKeyGate.cpp`).

*Gate, as built and measured.* Rev 2 specified containment **against the fixture interface map**
("every key the fixture tier-2 answers non-null must appear in the requested set"). W1's follow-up
measured that and it is **not a valid reference set** — 178 of 357 fixture-answerable keys are never
demanded, for three reasons, none of them a collection gap:

1. the fixture map is what Python's `address_selectors` **eagerly predicted** it might need — every
   participant's interfaces — not what its matchers read (a 4-pool DeDust route carries 4 pools; the
   engine looks up only the one `dedust_seq_destination` needs, and the un-queried pools are real
   participants, they appear in that trace's own action rows);
2. **probe artifacts**: the fixture's `jvault_assets` resolver parses *any* account's `data_boc`, so
   every NFT item "answers" that kind — all 6 such MISSING entries are NFT items, while every
   genuinely demanded jvault address has no NFT record;
3. with the production tier-1 map in place the count is strictly worse, since tier-1-servable keys
   never reach the hook at all. (The gate therefore runs **tier 1 empty**, which makes the dumped set
   the FULL demand set — the strongest form of the claim, nothing hidden behind an in-pass answer.)

So the reference set is **what Python actually consumed**: `twins/py_consumed.py` instruments
`TestInterfaceRepository` (both consumer families of §3.1 read through it — spec `lookup` nodes via
the registered resolvers, host fns directly) and runs py_classify's own loop over the corpus.
Resolver-internal reads are suppressed rather than recorded: `extract_jvault_assets` chains stake
wallet → pool `data_boc` → lock wallet, and the C++ side does that chain *inside* the resolver behind
one `jvault_assets` key (§6.2/D5), so recording its inner reads would demand keys the engine can
never ask for.

*Assertions.* **Cross-language containment — collect-pass demand ⊇ Python-consumed — asserted by
`py_consumed.py --compare`.** MEASURED: **185 consumed, gap = 0**, i.e. the demand-driven collect
misses nothing Python's matchers read; this is the evidence the "do not port address_selectors" claim
(§12.4) actually rests on. One EXTRA (`nft_auction`) goes the harmless direction and is inherent to
the design: the requested set is the union over ROUNDS, so it holds keys demanded only on the round-0
path where everything is still null — a getgems purchase asks `nft_auction` in round 0 because
`nft_sale` has not resolved yet, and stops asking once it has. Over-approximation costs a fetch;
under-approximation would be the bug.
The `--lookup-keys` mode asserts the collect pass's own invariants instead: **REQ-ODD = 0** (every
demanded key is a single `Str` address, so §3.2's kvrocks key derivation covers 100 % of demand),
**CASE = 0** (no uppercase/lowercase split on the demand side — W3 still owns the payload side),
no ERROR/TRACE_FAIL, and `rounds_exhausted = 0`. MISSING/SURPLUS against the fixture map stay in the
dump as a **reviewed artifact** (surplus is 7, all interface-less accounts — the legitimate class rev
2 anticipated).

**W3 — kvrocks backend, offline.** `EmuLookupActor` + `KvrocksConfig` reuse with the §5.3 explicit
connection options + per-kind JSON → `Value` decoders + the jvault chain (§6.2) + `destroyed`
per-table (§6.1) + `normalize_raw_address` on the key path (§3.2).
*Gate:* compile, plus a vector test in the `ton-mch-engine` test binary over payload fixtures
**generated by `KvrocksState`'s own builders (`KvrocksState.cpp:559-566` + per-table) or captured from
a real store** — never hand-built from this doc. Assertions: decoded `Value` equals the fixture
source's `Obj` for the same accounts; the address-case round trip of §3.2; one decimal-string
`int256`; one `destroyed: true` row treated as absent; one `latest_account_states` row confirming the
destroyed rule is correctly *not* applied there.

*Implementation split, as built.* The decoding is **pure** (`src/KvrocksDecode.{h,cpp}` in
`mch-classify`: payload string → `Value`, plus `kvrocks_resolve_batch` doing §6.5's grouping and the
§6.2 chain over an **injected** `PayloadFetch`), so the whole mapping is gateable with no store; the
redis++ transport is the only piece that needs one. `--kvrocks-decode-test` (ctest
`mch_kvrocks_decode`) is the vector gate.

*Why `build_payload` is exported* (lead ruling, recorded because it sits against CLAUDE.md's "never
modify main pipeline source for dev tools"). Offline there was no other honest source of real payload
bytes: all 19 builders were in an anonymous namespace, every payload-producing path runs through
`KvrocksBatchWriter` (needs a live `sw::redis::Redis&`), the repo carries no payload samples, and the
Python side only ever *reads* them (`indexer/core/kvrocks.py:213`). The rule targets bending pipeline
*behaviour or structure* to tooling convenience; this is a **visibility-only** change — six
declarations added to `KvrocksState.h`, the definitions moved out of the anonymous namespace
unchanged, zero behaviour delta — and the beneficiary is not a dev tool but a permanent regression
gate on a production wire contract: the C++ decoder must track that writer forever, and this gate is
the drift guard. The alternative (`#include "KvrocksState.cpp"` from a generator target) would have
encoded a fragile dependency on that module's internals — strictly worse coupling than an honest
export.

**W4 — async wiring.** Suspension state map, per-`trace_id` serialization (§5.2), the alarm sweep
(§5.5), the circuit breaker (§5.6), the suspended cap (§5.7), per-round deadline (§5.4), and the
`tier2{…}` stats group — including `wrong_value_guard`, whose *reject site* already landed in W1
(§8.3) and only needs its counter wired here.
**Also W4: raise `kMaxTier2Rounds` 4 → 6** (lead call, on W1 evidence). W1 forced the multi-round
path over the corpus with tier 1 emptied — the worst case, since production tier 1 answers in-trace
accounts in-pass — and got 61 / 85 / 17 / **2** fixtures converging at 1 / 2 / 3 / **4** passes, none
exhausted. Two traces landing exactly on the cap is no headroom for a limit whose overflow degrades
*silently* to last-pass-commit; both are multi-DEX chains (coffee + dedust + stonfi_v2), where each
stage's btype unblocks the next stage's lookups. `rounds_exhausted` stays watched in every live run.
*Gate:* compile; W1-W3 gates green; **plus a new unit gate** driving the classifier with a fake
`Tier2Fetch` that injects delay, error, and never-answers, asserting: exactly one `inner()` per
emission; `in_flight` returns to 0; same-`trace_id` emissions complete in order; an all-error run is
**byte-identical** to an all-null run. Plus code review of feature-off byte-neutrality.

**W5 — flags, capture, the local run.** The `--mch-kvrocks*` surface, startup validation, ~~tier-2
response recording in `--mch-capture` (§11.2)~~, the seeding script, and the §9.1 run.
*Gate:* the §11 live checks.

*As built (write-back W5 landed the flag surface):* `--mch-kvrocks{,-sentinels,-master,-user,
-password,-db}`, startup validation mirroring `ton-index-postgres/src/main.cpp:521-527`, the
`kLookupActors` pool wired through `lookup_pool_fetch` into `EmuShadowConfig::tier2_fetch`, and a
`tier-2 ENABLED` log line carrying the host only. **The capture item is deferred** — see §11.2 for
why (no reader exists, so the encoding has no ground truth); it moves to the capture-reader
half-wave alongside W-post. The seeding script and the §9.1 run are live work and remain open.

**W-post — the tier-1 interface-shape mini-wave.** Deliberately its own wave, landing **after** W4,
because every item in it edits files W4 holds open (`emu/EmuInterfaces.cpp`,
`emu/EmuClassifierActor.cpp`) and none of it is on the tier-2 critical path. Two queued items, both
already diagnosed, both carrying their own corpus coverage:

1. **§6.1a `nft_item.content`** — **LANDED**, all four touch points (tier 1, `to_v2`,
   `KvrocksDecode`, `abreplay/RedisReplay.cpp`). The flat-vs-nested question resolved as
   *unobservable*: nothing in the IR or in either serializer reads `meta`/`name`/`type` off the nft
   dict, and every corpus item's content is already flat `str->str` (e.g. `{"uri": …}`).

   *Measured, and the prediction below was WRONG — recorded because the error is instructive.* The
   row count behaved exactly as forecast: the 4 `nft_transfer` rows came back **in place of** the 4
   synthetic `unknown` rows (each affected trace carries exactly one action, so a 1:1 swap), total
   steady at **272**. But the forecast fatal total of **37 → 25** assumed the recovered rows would
   then compare CLEAN. They do not: they compare and hit the fixture-float artefact, which
   `ab_replay_diff.py` has no bucket for. Actual is **37 → 32** = 10 `py-only-row` (6 jvault + 4
   dedust, the tier-2 gap) + 5 `cpp-only-row` + 17 `real-mismatch`, of which 10 are those py-only rows
   re-counted and **7 are field diffs on `nft_transfer_data.{price,nft_item_index}`** of the form
   `'1037'` vs `'1037.0'` and `'7786…224'` vs `'7.786151643329036e+76'`.

   Those 7 are **dry-run-only** and must not be given a bucket. The fixture recorder stored those
   numbers as Python floats; prod Redis stores integers, and Python's own pending path reads the
   interfaces back out of the SAME `AddressInterfaces` blob we do
   (`interface_repository.py:58`), so both sides see an integer and render it identically. A real
   phase-1 sweep should show **zero** of them — adding tolerance for them would put a permanent hole
   in the gate that carries the contract risk, to hide an artefact that only exists offline.

   `--actions` / `--actions-prod` stayed byte-identical throughout, as required — neither exercises
   tier-1 `nft_item`.
2. **`change_dns` / `delete_dns`** — the bucket-B fills blocked on a `change_dns_record_data` column
   `mch::Action` does not have (`em2-writeback-design.md` §2.4). Adding it also means extending
   `scrub_arena_refs` in `emu/EmuClassifierActor.cpp`, which is the same W4-owned file, which is why
   these two travel together. A hermetic assertion currently pins `change_dns` as still-declining, so
   that expectation has to be flipped in the same change.

Neither is a regression of W1-W5; both are pre-existing gaps that the tier-2 work makes it sensible
to fix while the interface plumbing is fresh.

### 10.1 Composed CPU budget

Gating means a block's traces all wait on classification. Per em1-design §18: ~28 traces per block,
concurrent from the emulator's view but **serial inside one classifier actor**. With the fixpoint,
worst added block latency ≈ `28 × (rounds × classify_us + serialize_us)`; at the observed 13.3 ms tail
and 2 rounds that is on the order of **~1.5 s** against a 12-40 ms emulation. This is the number to
watch in the first live run — `classify_us{avg,max}` and the new `tier2{rounds}` make it directly
observable. Escape hatch, already designed and deliberately not built: em1-design Q9's N classifier
actors sharded by `hash(trace_id)`; `prep` is already shared const, and the per-`trace_id` queue of
§5.2 shards cleanly along the same key.

## 11. Verifying tier-2 for real

W1/W2 make the 165 fixtures cover the *mechanism*; the *data* needs:

1. **Miss-rate deltas.** `misses_by_kind` is already logged (`EmuClassifierActor.cpp:316-320`). Run
   the same window with and without `--mch-kvrocks`. Expected: `nominator_pool` and `jvault_assets`
   go from 100 % miss to non-zero `tier2` hits; `dedust_pool` misses collapse; `jetton_wallet` /
   `nft_*` misses drop by the counterparty share. A kind whose misses do not move is a wiring bug
   pointing at exactly one mapping.
2. **Hermetic capture-replay** — **DEFERRED out of W5 (lead-approved), bundled with the capture
   READER as one follow-up half-wave.** The intent is unchanged: extend `--mch-capture`
   (`emu/EmuCapture.cpp`, format in em1-design §7) with a `"tier2"` object recording every
   `(key → JSON payload | null)` fetched, so a replay reproduces a live classification with **no
   store at all**.

   Why it did not land with the rest of W5: **nothing in the tree reads capture files.**
   `--mch-capture` is write-only — `EmuCapture.cpp` and the flag itself are the only mentions, there
   is no reader, no replay mode and no twin — so the `Value`→JSON encoding has no ground truth to be
   correct against, and it only has one right answer once the reader's re-injection path exists.
   Writing it blind would mean the reader is later built to match whatever was guessed. The two
   therefore ship together, including the JSON→`Value` direction.

   **Not on the phase-1 critical path**: phase 1 replays prod-Redis HGETALL dumps through
   `mch-ab-replay` (`twins/README.md`), not capture files. The capture-replay is for making *live*
   disagreements reviewable offline afterwards, and for turning captured traffic into tier-2
   regression fixtures.
3. **Targeted protocol traces.** stonfi v1 swap (the `dex_in` reject), **Tonco deposit** (the §8.2
   wrong-asset case — the one that must now degrade rather than publish), jvault stake + claim,
   nominator payout, DeDust swap. Run each through `--mch-classify-only` and diff against the Python
   emulated classifier's `actions` field for the same Redis trace key.
4. **A/B on live traffic**, with the plan §3 caveat: Python runs the full-coverage engine vs mch's
   ported subset, so diffs must be bucketed "protocol-not-in-mch" before any mismatch number is read.
   Tier-2 makes this A/B meaningful for the first time.

## 12. Corrections to `emu-integration-plan.md` and `em1-design.md`

1. **plan §1.4b "batched kvrocks/DB round per collect pass"** — implies a trace-level collect pass
   exists. It does not; the fixpoint is per *(matcher, block)* inside `try_build`
   (`ClassifyCore.cpp:39-41` → `BuildDriver.cpp:235-254`), fetching synchronously at `:244`. §4 builds
   the trace-level pass.
2. **plan §1.4b "jvault needs a 3-round chained resolver — how the rounds compose"** — they do not
   compose with classify-level rounds. One `lookup jvault_assets(addr)` (`jvault.mch:103,160`), no
   `data_boc` kind (`BuildRuntime.cpp:226-228`) ⇒ the chain is internal to the resolver (§6.2).
3. **plan §1.4b "dedust_pool source = the shared `dedust_pools` map"** — that map is one branch of
   `get_dedust_pool`; `:300-306` reads the per-address kvrocks `dex_pools` payload, which the C++
   writer already populates (`KvrocksState.cpp:2903`). The map is a Python-process cache of a Redis
   blob written by a Python sidecar (`dedust_pools.py:40-77`). §6.3 takes the payload.
4. **plan §1.4b "address_selectors must NOT be ported"** — correct for the account-set predictors
   (`address_selectors.py:110-126`), which the collect pass replaces. But **one** function there is
   jvault lookup semantics rather than prediction and must be reimplemented in the resolver:
   `derive_accounts_from_stake_pool` (`:128-139`), whose derived `lock_wallet_address` the fixture
   source consumes pre-derived (`BuildRuntime.cpp:377-380`). ~10 lines on `load_address_py`.
5. **plan §3 EM2 "VERIFY whether ClassifyCore emits Python's unmatched-trace fallback"** — **it does.**
   `basic_classify_fallback` (`ClassifyCore.cpp:333-367`) invoked at `:529`; the emu actor already
   commits those rows (`EmuClassifierActor.cpp:115-116`). Strike it from the blocker list. It is now
   also load-bearing for §8.3.
6. **"kvrocks instances run on this box"** — they do not (§9.1); only `redis-server` on `:6379`. And
   the emulated-classifier unit sets no `TON_INDEXER_KVROCKS*`, so `--use-combined-repository` here
   resolves to the **PG** gather. "kvrocks first, PG fallback" describes production, not this host.
7. **em1-design §16.3 / Q7 — the "no per-request timer" decision is INVALIDATED.** Its stated ground
   was "the classifier does no I/O on this path (tier-2 is unwired)". Tier-2 wiring makes that false,
   and a suspended trace that never gets a response pins `finalized_blocks_inflight_` permanently.
   §5.5 revives the timer as an alarm-driven sweep scoped to suspended traces. em1-design §20's Q7
   answer should be marked superseded.
8. **em1-design §16.2's deadline statement** ("bounds the QUEUE WAIT only… tested once, at dequeue")
   is wrong once a request can wait on IO. §5.4 re-checks per round and states the composed worst case.
9. **New hazard, in neither doc:** suspension breaks per-trace emission ordering — harmless for
   EM1.5 stats, a correctness bug for EM2's write-back (§5.2). Mechanism: the `TraceInserter.cpp:88-95`
   finality-guard shape, plus classifier-side per-`trace_id` serialization.
10. **The existing fail-open watchdog cannot see a dead store** — it is gated on `in_flight >= 1024`
    while block budgets plateau the population near ~280 (§5.6). Two fixes there.

## 13. Resolutions

All rev-2 questions are closed. Recorded here so the wave reports have something to check against.

- **Q-A — scope of the wrong-value guard: ACTION-SCOPED** (lead, overruling the literal trace-scoped
  reading of the user's instruction). Rationale and the one-branch implementation in §8.3;
  `basic_classify_fallback` stays reserved for whole-trace classify failures.
  **Adjustment made while implementing it:** the proposed `FailureCategory::tier2_unresolved` is
  **withdrawn** — `ClassifyCore.h:23-36` documents that field as trace-terminal only and names
  `clean_reject` among the values deliberately excluded from it. The counter moved to
  `EmuTier2Stats::wrong_value_guard`, which is where that comment says per-(block, matcher, lookup)
  events belong. Same telemetry, no contradiction with the taxonomy.
- **Q-B — the guard also fires with tier-2 disabled: APPROVED** as a deliberate re-baseline. W1's
  corpus check stays; a resulting diff is documented in the wave report (§10 W1).
- **Q-C — `jvault.mch:160-172` non-early branch: APPROVED**, add the missing reject gate in W1 with
  the same fixture check (§10 W1 item 3). This resolves the one item §8.2 left open.
- **Q-D — the "always-null tier-2" gate variant: DROPPED** (§10 W1).
- **Q-E — the ~1.5 s worst-case block latency (§10.1): accept and measure** in the first live run;
  `classify_us{avg,max}` and `tier2{rounds}` make it directly observable, and em1-design Q9's
  sharding stays the designed-but-unbuilt escape hatch.

Still open, but as *work* rather than decisions: the item flagged inline at the end of §8.2 is now
Q-C's W1 task, and the S10 review point was resolved in §6.1 (one decoder shape; V4 resolves under
the ordinary `getgems_nft_sales` key — accepted by the lead on the code evidence).
