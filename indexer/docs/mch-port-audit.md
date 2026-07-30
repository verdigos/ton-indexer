# MCH Port Audit — the 66 unported Python matchers

Pre-flight capability audit of every Python matcher not yet ported to the `.mch` DSL, done
read-only on branch `merge-v13` @ `9a7d558`. Purpose: kill mid-wave surprises of the
`AnchorKind::Mixed`-was-never-implemented kind, and produce a batch plan for parallel
porting waves.

## 0. Baseline

| | count |
|---|---|
| Python matchers in `event_processing.py:213-306` (`dedust_v2.matchers()` expanded) | 100 |
| Python matchers already represented in `ir/mch_ir.json` | 34 |
| mch matchers in `ir/mch_ir.json` | 36 |
| **Unported** | **66** |

34 → 36 because three Python matchers each lower to two mch matchers
(`DedustDeposit*` → final/partial, `StonfiV2ProvideLiquidity` → final/partial,
`StonfiV2WithdrawLiquidity` → withdraw/withdraw_refund).

`FallbackJettonTransferBlockMatcher` (`matchers_for_failed_externals`) is out of scope —
it runs on a different entry path.

### Verified capability surface

Everything below was re-checked against source, not taken from prior notes.

- **AnchorKind** — `OpcodeSet`, `BType`, `Pred`, `Mixed` are **all implemented**
  (`ton-mch-engine/src/Walker.cpp:390-405`). The dedust_v2 wave closed the `Mixed` hole;
  no anchor kind blocks any remaining matcher.
- **Expr builtins** (`ton-mch-engine/src/ExprRuntime.cpp:628-653) — `account`, `amount`,
  `asset`, `ton_asset`, `addr_none`, `b64`, `first`, `last`, `len`, `sum`, `zip`, `map`,
  `concat`, `all`, `any`. No string manipulation of any kind.
- **Host fns** — 30 registered in `ton-mch-engine/src/HostRegistry.cpp`.
- **Lookup kinds registered** (`ton-mch-engine/src/BuildRuntime.cpp:210-212) —
  `jetton_wallet`, `nft_item`, `nominator_pool`, `jvault_assets`, `dedust_pool`,
  `nft_sale`, `nft_auction`.
- **Tier-1 emulator detectors** (`ton-trace-emulator/src/TraceEmulator.h:158-162) —
  JettonWallet, JettonMaster, NftItem, NftCollection, GetGemsNftFixPriceSale,
  GetGemsNftAuction, GetGemsNftFixPriceSaleV4, DedustPool, NominatorPool.
- **Tier-2 celldb kind gate** (`ton-mch-engine/src/emu/EmuCelldbLookup.cpp:64-68) — serves
  only `jetton_wallet`, `dedust_pool`, `nominator_pool`, `jvault_assets`. The three
  `nft_*` kinds are tier-1-only.
- **Message-schema leaf types** (`indexer/events/mch/msgs_gen.py:32-36) — `u32`, `u64`,
  `uint256`, `coins`, `addr_py`, `asset2`, `asset4`, `ref_cell`, `maybe_ref_cell`,
  `dict_keys267`, `fwd_cell_bits`, `fwd_cell_refs`. `bits N` is a **pure skip that emits
  nothing**. Missing: `u8`, `u48`, arbitrary `uN`, 1-bit bool, raw-bytes-N.
- **ActionBuild fill coverage** (`ton-mch-engine/src/ActionBuild.cpp:731-780) — 31 btypes.
- **Action composite fields** (`ton-mch-engine/src/ActionBuild.h:28-32) — 15, listed in §2.4.

## 1. Headline

**No engine work is on the critical path.** Every genuine language gap found has a
host-function escape hatch, and the highest-value wave needs none of them.

Class counts over the 66 (a matcher can carry several tags):

| class | count | meaning |
|---|---|---|
| **DECL** | 47 | pattern + build expressible today; cost is spec + parser + serializer only |
| **HOSTFN** | 15 | needs ≥1 new host fn or predicate |
| **PARSER** | 38 | needs a hand parser or an ABI row (schema leaf set too small) |
| **SERIALIZER** | 64 | needs a new C++ `fill_*`; 25 of those also need a new composite `Value` field |
| **BUILTIN** | 3 | needs a string `contains` — all three can dodge it with a host predicate |
| **ENGINE** | 4 | touches a real language gap; **all four have a host-fn workaround** |
| **LOOKUP-BLOCKED** | 3 | the emulator physically cannot supply the interface — Multisig only |

Only two matchers of 66 (`jetton_transfer`-anchored `CoffeeStakingDeposit`, and
`CoffeeCreatePool`) are blocked on another *matcher* landing first.

### Observed volume

Decoded from the live A/B snapshots at `/home/verdigos/dev/em-ab/`:

| type | snapB5 (6 616 traces, 16 466 rows) | snapB4 (4 571 traces, 10 200 rows) |
|---|---|---|
| `teleitem_start_auction` | 834 | 384 |
| `teleitem_cancel_auction` | 28 | 12 |
| `nft_mint` | 22 | 56 |
| `dedust_v2_claim_fees` | 5 | — |
| `nft_cancel_sale` | — | 4 |
| `nft_cancel_auction` | 1 | — |
| `nft_put_on_auction` | — | 1 |
| `cocoon_proxy_charge` | 1 | — |

Unported-attributable rows in the B5 window: **~891 of 16 466 (5.4 %)**. Of those,
**884 (99 %) come from exactly three matchers**: `TeleitemStartAuction`,
`TeleitemCancelAuction`, `NftMint`.

Caveat worth stating plainly: these are sampled emulation windows and Fragment auction
traffic is bursty. But two independent snapshots agree on the ranking, and the same three
matchers top both. Everything else in the backlog is rare-to-invisible at this sample size.

## 2. Cross-cutting findings

### 2.1 `parent_action_id` / `ancestor_type` — a real gap, but an *accepted* one

C++ `Action` (`ton-mch-engine/src/ActionBuild.h:20-32`) has neither field, and
`ActionBuild.cpp:27` declares the `v1_ops` wrapper explicitly out of scope. Python's
`serialize_blocks` (`indexer/events/blocks/utils/block_tree_serializer.py:1664-1678`)
recurses `children_blocks` into child rows for any btype **not** in `v1_ops` (`:1601-1639`).

This affects roughly 30 of the 66. It is **not a porting blocker**, because four already-
shipped mch matchers (`dedust_v2_claim_reward`, `coffee_create_vault`, `tgbtc_mint`,
`layerzero_receive`) are equally absent from `v1_ops` and shipped anyway. It caps
row-for-row parity on those btypes, it does not prevent the parent row from being emitted.

Decide once, at program level, whether to close it. Do not re-litigate it per wave.

### 2.2 `pre_build_auto_append=True` — has a shipped workaround

`basic_matchers.py:49-56`. The C++ engine implements only the `False` form
(`ClassifyCore.cpp:97-113`). Exactly three Python matchers set it: `staking.py:328`
(NominatorPoolDeposit), `:354` (NominatorPoolWithdrawRequest), `:433`
(NominatorPoolWithdraw).

**`:433` is already ported.** `specs/nominator.mch:1-24` documents the escape: re-anchor
per payout and check the parent through a host predicate on `.previous_block`, so the
parent is never in the consumed set. Same trick applies to the other two. Downgrade this
from ENGINE to "known pattern".

### 2.3 Trace post-processors — the stage exists

`post_process_dedust_v2_liquidity` is a trace post-processor, not a matcher, and the
dedust_v2 deposit family hard-depends on it. The C++ engine **has** the stage:
`ClassifyCore.cpp:119` ("Phase E"), invoked at `:396-398` and `:545-547`, currently running
`post_process_dedust_liquidity`, `post_process_stonfi_v2_liquidity`, `unwind_deployments`.
Cost of the v2 pass is one new C++ function plus two registration lines — **not** a new
engine stage. Do not try to parameterize the v1 pass; the merge keys and semantics differ.

### 2.4 New composite `Value` fields — 25 needed

Absent from `ActionBuild.h:28-32`: `nft_mint_data`, `multisig_create_order_data`,
`multisig_approve_data`, `multisig_execute_data`, `vesting_add_whitelist_data`,
`evaa_supply_data`, `evaa_liquidate_data`, `tonco_deploy_pool_data`,
`coffee_create_pool_data`, `coffee_staking_deposit_data`, `coffee_staking_withdraw_data`,
`layerzero_send_data`, `layerzero_dvn_verify_data`, and 12 `cocoon_*_data`.

Postgres DDL already exists for all of them (`ton-index-postgres/src/migrate.cpp:290-413`,
`:909-923`; `indexer/core/database.py`), so this is C++-side plumbing only, four touchpoints
each (`ActionBuild.h`, `Classify.cpp`, `emu/EmuActionSerialize.cpp`,
`emu/EmuClassifierActor.cpp`). **No migration.**

Operational hazard, per prior experience: adding a `Value` field requires a *clean* engine-obj
rebuild (ninja skips generated TUs and you get a heap-corruption segfault), and the `VType`
enum must be appended, never inserted. Batch composite additions into as few rebuilds as
possible.

### 2.5 Message parsers are the dominant hidden cost

38 of 66 need parser work. The schema leaf set cannot express `u8`, `u48`, arbitrary `uN`,
1-bit bools, or raw byte blobs, and `bits N` skips rather than emits. Three routes:

1. **ABI row** (`ton-abi/abi/*.tolk`) — best for DeFi protocols; Tolk has `bool`, `int24`,
   `uint128`, `uint160`. Bridge rows at `AbiBridge.cpp:150-152`.
2. **Hand parser** in `ton-mch-engine/src/parse/` + a row in `MsgParse.cpp:56-67` — required
   whenever parsing must be *tolerant* (TgBTC burn logs truncate addresses and Python
   catches it; a schema `addr_py` would hard-fail the whole parse) or must emit a formatted
   string (LayerZero status maps, TgBTC minimal-hex pubkeys).
3. **One `uN` leaf in `msgs_gen.py`** — would unblock Cocoon's `uint2` and several others.
   Cheapest single generator change available.

Coffee is the happy exception: **zero parser gaps**. `asset2` already is the coffee asset,
and every 1-bit flag it carries is dropped unread, so `bits N` covers them.

### 2.6 `nft_item.code_hash` is missing from all three C++ lookup sources

`ParsedBlockLookupSource.cpp:88-99`, `fixtures/FixtureLookupSource.cpp:57-68`,
`emu/EmuInterfaces.cpp:15-29` all emit `{address, init, index, collection_address,
owner_address, content}`. Python's `NFTItem` has `code_hash`
(`interface_repository.py:147`). Reading `nft.code_hash` faults at
`ExprRuntime.cpp:342-345`.

`schema::NFTItemDataV2::code_hash` exists (`IndexData.h:653`) but
`NftItemDetectorR::Result` does not carry it (`Tokens.h:77-96`), so the emulator path must
compute it from the code cell at the `to_v2` callsite. Three-file change; unblocks
`DnsRelease`, `DnsPurchase`, `AuctionBid`, `NftPutOnSale`. Same omission applies to the
`nft_sale` / `nft_auction` shapes.

### 2.7 Two non-gaps, confirmed

- **External-out log messages are matchable.** `SchemaTraceLoader.cpp:109-113` maps every
  `out_msgs` entry with no destination filter; `BlockTree.cpp:113-117` wraps a
  destination-less message as a real leaf `EventNode`; `:412-424` gives it
  `btype == "call_contract"` with `opcode` set; `Walker.cpp:390-393` anchors on it. All four
  TgBTC `*LogOnlyMatcher` variants are portable today. This was the biggest suspected
  engine gap in the backlog and it does not exist.
- **The `dns_entry` note** at `EmuInterfaces.cpp:27` refers to `NFTItemDataV2::DNSEntry`
  (resolver records). Neither DNS matcher reads it. Red herring.

### 2.8 Multisig is the only hard lookup block

`get_multisig_order` has no registered kind, `MultisigOrder` is absent from the emulator's
detector set (it exists only in `index-engine/src/BlockInterfacesDetector.h:12`), the tier-2
gate does not serve it, and Python's own `EmulatedRepository.get_multisig_order` returns
`None` unconditionally (`interface_repository.py:595-597`). All three Multisig matchers
yield zero rows on the emulated path until `MultisigOrder` is added to `TraceEmulator.h`
**and** the celldb kind gate. Treat as its own preparatory task or drop the family.

## 3. Per-matcher table

Classes: **D**=DECL, **B**=BUILTIN, **H**=HOSTFN, **E**=ENGINE, **L**=LOOKUP gap,
**P**=PARSER, **S**=SERIALIZER (`S+` = also needs a new composite `Value` field).
"Rows" = observed in the snapB5 window unless noted.

### NFT / marketplace / auction (11)

| matcher | class | blockers / notes | rows |
|---|---|---|---|
| `TeleitemStartAuction` | D B L P S | `contains` on the Fragment URI (or a 4-line predicate — C++ already does this test at `HostNft.cpp:225-228`); 1 schema row; `nft_item` tier-1 only. **Telemint interface NOT needed.** Fills `nft_transfer_data` + `nft_listing_data`, both exist. No DDL. | **834** |
| `TeleitemCancelAuction` | D B L S | same predicate; no new parser; shares `_fill_cancel_nft_trade` with three others | **28** |
| `NftMint` | D L S+ | Mixed anchor over `call_contract`/`ton_transfer` + deploy predicate (`orig_status != active && end_status == active`, both in the envelope). Needs **`nft_mint_data`** composite. Widest anchor in the set — watch scan cost. Also unblocks `CoffeeStakingDeposit` and the already-shipped-but-dead `tonco_deposit_liquidity`. | **22** (56 in B4) |
| `UpdateSale` | D L P S | the most declarative matcher in the whole backlog; `nft_sale` fields all present | rare |
| `NftDiscovery` | D P S | **zero lookups** — no interface risk at all | rare |
| `NftCancelSale` | D L S | shares the cancel fill | 4 (B4) |
| `NftCancelAuction` | D L S | Mixed anchor + `produces switch`; textbook case | 1 |
| `NftFinishAuction` | D L S | must register after `getgems_nft_purchase` (already ported) | rare |
| `AuctionBid` | H L S | needs `code_hash` (§2.6); two-branch getgems/DNS — cleanest as two matchers | rare |
| `AuctionOutbid` | B E S | **ENGINE: "exactly one, reject on duplicate"** has no quantifier. Escape: list binding + `reject when len > 1`. Depends on `AuctionBid`. | rare |
| `NftPutOnSale` | H L E S | **heaviest in the family.** Parses GetGems StateInit with per-code-hash version dispatch → needs `init_state` on the mch `Message` (absent from `TraceLoader.h:30-56` and never read by `SchemaTraceLoader`). **Cheaper route: widen the tier-1 `nft_sale`/`nft_auction` shapes** — every field it parses is already on the detector structs (`IndexData.h:676-724`), which would make it pure DECL and delete the engine work. Decide before anyone plumbs `init_state`. | 1 (B4) |

### DNS / subscriptions / elections (6)

| matcher | class | blockers / notes | rows |
|---|---|---|---|
| `Unsubscribe` | D S | smallest port in the backlog; 3-line fill, no composite | rare |
| `ElectionDepositStake` | D S | zero lookups, zero tick-tock/config dependency (elector address is a literal). Address gate must be `reject when`, not `where` — `where` reads block data only (`ExprRuntime.cpp:378-394`) | rare |
| `ElectionRecoverStake` | D S | shares one 2-line fill with the above | rare |
| `Subscription` | D P S | write as `@resp -> { @pay } <- maybe @req`; the flat form trips `C009_MIXED_CHILD_PARENT_CHAIN_UNSUPPORTED`. Message is schema-generatable. | rare |
| `DnsRelease` | D L P S | blocked on `code_hash` (§2.6); hand parser (conditional-length `query_id`) | rare |
| `DnsPurchase` | H L P S | **the matched parent is deliberately NOT consumed** (`auction.py:583`) and the DSL always consumes its match. Escape: drop the parent edge, add a host fn reading `.previous_block` — exact precedent `nominator_pool_withdraw_parent`. **The fill is free**: Python routes it to `_fill_nft_purchase_action` and C++ `fill_nft_purchase` already exists — one dispatch line. (The "bucket C, no matcher produces it" comment at `ActionBuild.cpp:775` goes stale the moment this lands.) | rare |

### Staking / multisig / vesting (8)

| matcher | class | blockers / notes | rows |
|---|---|---|---|
| `VestingAddWhiteList` | D P S+ | cheapest of the eight; reuses `vesting.mch`'s two-node shape 1:1. Hand parser (unbounded ref chain, one address per level). Needs `vesting_add_whitelist_data`. | rare |
| `NominatorPoolDeposit` | D L S | lookup is a **drop-in** (registered, tier-1, tier-2-served, Python resolver already written). Only friction is auto-append (§2.2). | rare |
| `TONStakersDeposit` | D S | **TRAP:** the optional `MintJettons` branch is dead — Python raises `AttributeError` at `staking.py:170` when it is absent, and `try_build` swallows it. A literal port null-propagates and emits a block Python never emits. Add `reject when transfer == null` and drop the `maybe`. | ~5 (`stake_deposit`) |
| `TONStakersDelayedWithdrawal` | H P | **zero serializer work** (`fill_tonstakers_withdraw` exists, fields match 1:1). Needs a host fn for the unbounded `previous_block` pool-address walk. Only ~15 % reuse from `tonstakers.mch` — sibling in name only, inverted pattern direction. | rare |
| `NominatorPoolWithdrawRequest` | H L E | **zero serializer work.** ~60 % reuse from `nominator.mch`, but the address direction is inverted. **ENGINE: the ≥3-transfer bail** — a `maybe {t1; t2}` children set greedily eats 2 of 3; needs a host predicate counting `next_blocks` or an exact-arity quantifier. Canonical ascending-`min_lt` candidate order (spec §10.5) retires the `min(key=created_lt)` sort for free. | ~3 |
| `MultisigCreateOrder` | L P S+ | **LOOKUP-BLOCKED (§2.8).** Plus missing leaves (`u8`, `u48`, 1-bit) and a new composite. Also: all three Multisig fills push lookup-derived signer addresses into `action.accounts`, and C++ accounts assembly is source/destination-driven only (`ActionBuild.cpp:823-847`) — needs a fill→extra-accounts channel. | rare |
| `MultisigApprove` | H L P S+ | **LOOKUP-BLOCKED.** Mixed anchor is fine (spec §8.3 names this matcher as the motivating case). Needs a host fn for the *fallible* body parse — DSL `parse` failure rejects the match. | rare |
| `MultisigExecute` | L P S+ | **LOOKUP-BLOCKED.** Worst parser of the three: needs raw-bytes-32 emission for `signers_hash`, which no leaf provides. | rare |

### DeFi remainder (10)

| matcher | class | blockers / notes | rows |
|---|---|---|---|
| `DedustV2ClaimFees` | D P S | ~90 % clone of the shipped `dedust_v2_claim_reward.mch`, 2 legs instead of 1; composite exists | **5** |
| `EthenaWithdrawalRequest` | D L S | linear 4-chain, `jetton_wallet` lookups both registered and tier-2-served; reuses `staking_data`. Verify `0xb2583ed5` can be parsed as `JettonInternalTransfer`. | rare |
| `ToncoDeployPool` | D P S+ | simplest shape in this group; cost is entirely the `tonco_deploy_pool_data` composite + 2 ABI rows (`int24`/`uint160`/`bool`) | rare |
| `DedustV2Withdraw` | D P | **zero serializer work** — `fill_dex_withdraw_liquidity` matches key-for-key. 2 ABI structs. | 2 |
| `DedustV2DepositPartial` | D P S | needs the new post-process pass (§2.3); new fill branch (Python's partial fill differs from C++ `fill_dedust_deposit_partial`) | 3 |
| `DedustV2Deposit` | H P S | host fn for the deposit payload (twin of the existing `dedust_v2_swap_payload`); **owns the new post-process pass** | 3 |
| `DedustV2RejectedDeposit` | D | free if bundled with `DedustV2Deposit` — shares every parser and the host fn | rare |
| `ToncoWithdrawLiquidity` | H L S | one host fn over `consumed` (pTON unwrap, router fallback, payout reordering) — precedent `tonco_deposit_liquidity_data`. **High `HostTonco.cpp` reuse.** Adds 4 tonco-only keys to the existing composite. | rare |
| `EvaaSupply` | H P S+ | 3 host fns (1 header predicate + 2 cell-descent), 1 hand parser, new `evaa_supply_data` | rare |
| `EvaaLiquidate` | H P S+ | **heaviest matcher in the backlog.** Deepest XOR tree, two branches sharing a head opcode, mixed anchor, 3 host fns, hand parser with an error-code→string map, new `evaa_liquidate_data`. | rare |

### Coffee (9) — zero parser gaps, zero lookups

| matcher | class | blockers / notes | rows |
|---|---|---|---|
| `CoffeeWithdrawLiquidity` | D P | **zero serializer work.** `stonfi_v2_withdraw.mch:39-70` is a near drop-in template — same `jetton_burn` anchor, same positional payout binding. 1 schema row. | moderate |
| `CoffeeDepositLiquidity` | D P S | one dispatch line (`fill_dedust_deposit` is field-for-field identical to the Python fill). 4 schema rows. Template = `dedust_deposit.mch`. | moderate |
| `CoffeeStakingClaimRewards` | D P S | flattest of the nine | rare |
| `CoffeeStakingWithdraw` | D P S+ | three messages share opcode `0xcb03bfaf`, disambiguated purely by depth — legal, the registry keys by name | rare |
| `CoffeeMevProtectHoldFunds` | D P S | uses Mixed anchor + the `any` node + parent-with-children — all three already implemented | rare/moderate |
| `CoffeeMevProtectFailedSwap` | H P S | parses a jetton `forward_payload`, and `parse` only reads the block's own body. Either one tiny host fn or add `parse EXPR as T` to the DSL (broader payoff). | rare |
| `CoffeeCreatePoolCreator` | D P S | the naive port needs a forward-payload host fn, but **the anchor body `0xc0ffee23` already carries every field** — verify `anchor.body.sender == jt.data.sender` on real traces and the host fn disappears | rare |
| `CoffeeCreatePool` | D P S+ | anchors on `CoffeeCreatePoolCreator`'s product — same wave, registered after | rare |
| `CoffeeStakingDeposit` | D S+ | **blocked on `NftMint`** (`btype nft_mint` has no producer in the IR). Bare `btype jetton_transfer` anchor — widest in the family. | rare |

### TgBTC (6) — all portable, log-only confirmed fine (§2.7)

| matcher | class | blockers / notes | rows |
|---|---|---|---|
| `TgBTCMintLogOnly` | D | **serializer is FREE** — same `tgbtc_mint` btype, and `fill_tgbtc_mint` already reads `crippled` and appends `_fallback`. Reuses `HostTgbtc.cpp` helpers. Smallest port here. | rare |
| `TgBTCDkgLogOnly` | D P S | note Python has no non-fallback `tgbtc_dkg_log` matcher — this btype exists only via the log path | rare |
| `TgBTCNewKeyLogOnly` | D P S | parser + fill shared with `TgBTCNewKey` | rare |
| `TgBTCBurnLogOnly` | D P S | parser must be **tolerant** (Python catches truncated addresses → `AccountId(None)`); a schema `addr_py` would hard-fail | rare |
| `TgBTCBurn` | D P S | use `^` not `|` — the duplicated capture across branches is only legal under exclusivity (R005). Ternary covers the `min`/zero-fallback; no host fn. | rare |
| `TgBTCNewKey` | D P S | `op @k <- any @p -> { op @dkg }` is legal (not the C009 shape) | rare |

### LayerZero (4)

| matcher | class | blockers / notes | rows |
|---|---|---|---|
| `LayerZeroCommitPacket` | D P S | needs zero new parse primitives — `lz_parse_packet`/`lz_hex_min` in `ParseLayerZero.cpp` drop straight in. **Trap:** `msglib_callback` is declared optional but hard-required in `build_block` → port as `maybe` + `reject`. Preserve the `uln`/`uln_connection` double-assignment quirk verbatim. | rare |
| `LayerZeroDvnVerify` | D P S+ | hand parser (u116/i234 widths, status→string map incl. `unknown_{code}`); new `layerzero_dvn_verify_data` | rare |
| `LayerZeroSend` | D P S+ | biggest parser in the backlog (`MDLzSend`: u80/u180/i90/u128 widths); new `layerzero_send_data` | rare |
| `LayerZeroSendTokens` | D S | no parser at all; anchors on `LayerZeroSend`'s product — ship together. Replicate the `source` double-write order (jetton sender wins). | rare |

### Cocoon (12) — recommend DEFER

Structurally the **cheapest** family in the backlog: all opcode-anchored, all depth 2-3, no
recursion, no alternatives, no `where` predicates, **zero lookups**, and every fixture
(`tests/test_cases/cocoon.yaml`, 13 cases) has its `.lz4` present.

But the cost is 12 new composite `Value` fields × 4 serializer touchpoints each (~48
mechanical edits), 3 dual-language host fns, one `uN` schema leaf, and 12 unresolved
child-action gaps — for **one observed row in 6 616 traces (0.015 %)**. Worst
effort-per-row ratio remaining.

If anything ships: **`cocoon_proxy_charge` + `cocoon_grant_refund` only** — 2 matchers, 3
schema-clean parsers, 1 shared host fn (extern-address string form), 2 composites of one
identical shape. That subset carries 100 % of observed traffic.

Two family-specific parity traps if it is ever ported: `msg.opcode` is **signed** as stored
(`ExprRuntime.cpp:279`) while `block.data.opcode` is unsigned — never compare
`x.msg.opcode == 0xF5F26A36`; and `failed` must come from `x.msg.transaction.aborted`, not
`x.failed`, because `CallContractBlock.failed` is force-cleared for zero-value calls.

Separately: cocoon composites are currently in the broken set of the Python-writer wire-type
bug (`docs/em2-change-inventory.md:652-655`), so cocoon traces vanish from the pending
endpoints today. That is fixable by adding cocoon int fields to the existing `int_keys` sets
— it does not require porting any matcher.

## 4. Batch plan

Ordered by row recovery. **PARALLEL-SAFE** = disjoint opcode/registry/fill surface from the
other waves marked the same; can run concurrently. Serial dependencies are called out.

### Prep tasks (small, unblock several waves — do these first or fold into Wave 1)

| id | task | unblocks | parallel |
|---|---|---|---|
| **P1** | string `contains`/`startswith` expr builtin — *or* accept one small host predicate reusing `HostNft.cpp:225-228` | Teleitem ×2, AuctionOutbid | yes |
| **P2** | `code_hash` on the tier-1 `nft_item`/`nft_sale`/`nft_auction` shapes (3 files: `ParsedBlockLookupSource.cpp`, `emu/EmuInterfaces.cpp`, `fixtures/FixtureLookupSource.cpp`) | DnsRelease, DnsPurchase, AuctionBid, NftPutOnSale | yes |
| **P3** | **decision only**: close `parent_action_id`/`ancestor_type` or accept the gap program-wide (§2.1) | ~30 matchers' full parity | n/a |
| **P4** | **decision only**: widen tier-1 `nft_sale`/`nft_auction` shapes vs. plumb `init_state` (§3, `NftPutOnSale`) | NftPutOnSale | n/a |

P1 and P2 are each a few hours and are the only prep work that pays for itself immediately.

---

### Wave 1 — Teleitem + NftMint  ·  **~884 rows/window (99 % of recoverable)**  ·  PARALLEL-SAFE

`TeleitemStartAuction`, `TeleitemCancelAuction`, `NftMint`

Needs P1. One schema message row, `nft_item` lookup (tier-1 present, no tier-2 work), two
C++ fills (`nft_put_on_auction`, `cancel_nft_trade`), one new composite (`nft_mint_data`),
one `btypes.py` row each. No engine work, no Telemint interface, no DDL beyond the composite.

This wave alone is worth more than the other eight combined. Do it first, do it alone if
capacity is tight.

### Wave 2 — NFT declarative tail  ·  low rows  ·  SERIAL after Wave 1

`UpdateSale`, `NftDiscovery`, `NftCancelSale`, `NftCancelAuction`, `NftFinishAuction`

All pure DECL. Serial only because three of them share the `_fill_cancel_nft_trade` fill
that Wave 1 introduces; `UpdateSale` and `NftDiscovery` are independent and could move into
Wave 1 as filler. `NftFinishAuction` must register after the already-ported
`getgems_nft_purchase`, and its anchor overlaps `NftCancelAuction` — pin the same relative
`priority` Python has.

### Wave 3 — Coffee liquidity  ·  moderate real DEX rows  ·  PARALLEL-SAFE

`CoffeeWithdrawLiquidity`, `CoffeeDepositLiquidity`

Extends `coffee_liquidity.mch`. One schema batch (4-5 rows, all leaves exist), **one C++
dispatch line**, zero new composites, zero DDL, zero host fns, zero lookups. Both btypes are
in `v1_ops` so there is no child-action gap at all. 19 fixtures already exist
(`tests/test_cases/coffee.yaml:352-604`).

Lowest-risk work in the entire backlog and it targets live DEX traffic. Best candidate to
run concurrently with Wave 1.

### Wave 4 — dedust_v2 completion  ·  ~10 rows  ·  PARALLEL-SAFE, serial internally

`DedustV2ClaimFees` → `DedustV2Withdraw` → (`DedustV2DepositPartial` + `DedustV2Deposit` +
`DedustV2RejectedDeposit` + the new post-process pass, as one unit)

ClaimFees and Withdraw are independent and cheap (ClaimFees is a 90 % clone of a shipped
spec; Withdraw needs **zero** serializer work). The deposit trio must ship together with the
Phase-E pass (§2.3). ABI rows go in `dedust_v2.tolk` — note `indexer/dedust_abis/` is **not
present on merge-v13**; fetch it before writing struct decls.

### Wave 5 — cheap singles  ·  low rows  ·  PARALLEL-SAFE, splittable into two

**5a:** `Unsubscribe`, `ElectionDepositStake`, `ElectionRecoverStake`, `Subscription`
**5b:** `TONStakersDeposit`, `VestingAddWhiteList`, `EthenaWithdrawalRequest`, `NominatorPoolDeposit`

All DECL, disjoint registries, no shared fills. 5a has **zero lookups and zero host fns** —
the purest DSL exercise available, good for onboarding. 5b: watch the `TONStakersDeposit`
dead-`maybe` trap, and `NominatorPoolDeposit` needs the §2.2 re-anchor pattern.

### Wave 6 — TgBTC  ·  low rows  ·  PARALLEL-SAFE, serial internally

**6a:** the four `*LogOnly` matchers — one new `src/parse/ParseTgbtc.cpp` covering all four
event bodies, one `specs/tgbtc_logonly.mch`, three new fills (mint's is free).
**6b:** `TgBTCBurn`, `TgBTCNewKey` — pure shape work, reuses every 6a parser and fill.

### Wave 7 — Tonco + zero-serializer staking  ·  low rows  ·  PARALLEL-SAFE

`ToncoDeployPool`, `ToncoWithdrawLiquidity`, `TONStakersDelayedWithdrawal`

Grouped because all three are "one host fn or one composite, nothing else".
`TONStakersDelayedWithdrawal` and `ToncoWithdrawLiquidity` need **zero serializer work**.

### Wave 8 — LayerZero  ·  low rows  ·  PARALLEL-SAFE, serial internally

**8a:** `LayerZeroCommitPacket` (needs no new parse primitives) + `LayerZeroDvnVerify`
**8b:** `LayerZeroSend` + `LayerZeroSendTokens` — must ship together; biggest parser in the
backlog plus one new composite.

### Wave 9 — hard tail  ·  near-zero rows  ·  mostly SERIAL

`AuctionBid` → `AuctionOutbid` (needs P2 + the exactly-one workaround);
`NftPutOnSale` (needs P4 decided);
`DnsRelease` + `DnsPurchase` (need P2);
`NominatorPoolWithdrawRequest`;
`CoffeeStakingDeposit` (**after Wave 1's NftMint**) + `CoffeeStakingWithdraw` +
`CoffeeStakingClaimRewards`;
`CoffeeCreatePoolCreator` → `CoffeeCreatePool`;
`CoffeeMevProtect` ×2;
`EvaaSupply` → `EvaaLiquidate` (heaviest pair in the backlog).

### Deferred

- **Multisig ×3** — LOOKUP-BLOCKED (§2.8). Port only if `MultisigOrder` is first added to
  the emulator detector set *and* the celldb kind gate. That prep is its own task and
  benefits nothing else.
- **Cocoon ×12** — defer the family. Ship `cocoon_proxy_charge` + `cocoon_grant_refund`
  only if cocoon coverage is ever demanded.

---

### Recommended next three waves

1. **Wave 1 (Teleitem + NftMint)** — 99 % of recoverable rows, needs only P1.
2. **Wave 3 (Coffee liquidity)** — real DEX traffic, lowest risk in the backlog, fully
   parallel with Wave 1.
3. **Wave 4 (dedust_v2 completion)** — closes a family that is already half-shipped and
   whose infrastructure is 90 % reusable; parallel with both of the above.

## 5. Test coverage

Fixtures exist for Coffee (19 cases) and Cocoon (13 cases, all `.lz4` present). The six
DNS/subscription/election matchers have **no** YAML cases and no corpus btypes — every one
of them needs fresh fixtures before it can be A/B-gated. Budget fixture capture into Wave 5a
explicitly.
