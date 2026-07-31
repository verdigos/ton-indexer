# Frozen action goldens

`actions.golden` is the committed expectation for

```
ton-mch-engine --actions -T indexer/tests/traces
```

run from the repo root (no `-I`: the matcher table is compiled in since EM4).
**197 fixtures, 861 ACTION rows** — one per serializable block on each trace's
post-pass spine, plus the CHILD rows the serializer expands out of any spine
block whose btype is outside `v1_ops`, plus 6 synthetic `unknown` rows (the
fixtures whose spine serializes to nothing at all). Rows are printed **sorted**
within a fixture, because spine order is not a stable property (see below).

## Why it exists

It replaces the live Python twin (`twins/py_actions.py`) as the reference for
the fixture actions gate. The twin imports the real Python classifier, so it
dies with it — and Python's `--emulated-traces` path is being retired. This file
is the last snapshot taken while both engines could still be compared.

## Provenance

Re-baselined 2026-07-31 (**886 → 902 rows, 208 → 211 fixtures**) by the EVAA
liquidation wave. NOT pure-addition this time: Stage A added the
`evaa_liquidate_data` action composite, which mechanically inserts
`evaa_liquidate_data=null` into every pre-existing row (verified byte-identical
modulo that insertion, held-out re-diff on the 208 old fixtures). 3 snapshot-mined
fixtures (incl. one success=false). Port is bug-for-bug per porting-tail-plan §9
A3: modern jetton-heavy liquidations stay UNMATCHED on both sides (14 known
negative dumps verified, 3 spot-checked against both engines) — the legacy
modern-shape gap is a separate parked product issue. Wave also fixed a real
parity gap found in verification: `debt_asset_id` never populated by the port's
first cut, and C++ `rt_access_build` faults (rejects the build) on missing dict
keys where Python nulls gracefully — silent row-drop until the host fn carried
the field.

Re-baselined 2026-07-31 (**866 → 886 rows, 202 → 208 fixtures**) by the DNS
auction tail wave: `dns_purchase` + `dns_release` matchers (specs/dns.mch) with
6 live fixtures — 3 purchase (snapshot-mined via actions table; opcode-only
hunts surface teleitem look-alikes since `AuctionFillUp` 0x370FEC51 is shared,
gated here by the nft_item code-hash/collection guard) and 3 release. All 20 new
rows additive; teleitem negative-check verified (look-alike trace emits no
dns_purchase). Includes legacy `_fill_dns_release` dual-access (`_dget`) fix —
the fill was unreachable via dict-shaped data until the declarative producer
landed.

Re-baselined 2026-07-31 (**861 → 866 rows, 197 → 202 fixtures**) by the
subscriptions wave (35af75d): `subscribe` + `unsubscribe` matchers
(specs/subscriptions.mch, fully declarative) with 5 live fixtures — 3 subscribe,
2 unsubscribe, hunted by wallet-plugin opcodes from cold prod. All 5 new rows
additive. Uncovered branches (no live specimen found): subscribe `failed=true`,
unsubscribe null-beneficiary.


Re-baselined 2026-07-31 (**838 → 861 rows, 184 → 197 fixtures**) by registering
13 live nominator-pool fixtures (deposit + withdraw_request + op-0x2
double-payout), the first fixture coverage for `specs/nominator.mch`'s deposit
and withdraw_request matchers (previously 0 rows of either). All 23 new rows are
additive — the pre-existing 838 rows are byte-identical (verified: the diff
against the old golden contains only pure-addition hunks, no changed lines).

The fixture stems, grouped by live shape, are:

- **deposit:** `1DtgStJMB5JNE8vxQD2SYG1E4DO3a6nsUQlA2y-y_E4=`,
  `7-PBki6rgQj5bmiG-BxVXPDpkbSM1TucbB83OFr2CC4=`
- **deposit-bounced:** `XrJ4BgG2rSQyr3vItrDD4kFQZMU0v17UGRD3LQPvuOA=`,
  `awhk7c9wvvH3jR3PrLJbd4sqrfpRlHRwjvlwuIGtPXk=`
- **withdraw-request-zero-payout:**
  `CwcLdAHHpMSrEbbd_9S7YRvBreXTQHSGIov5Mbv39KA=`,
  `59b4F0GX09WQKfMi2v09JdqqO1eab4fH8YPSrGqujnM=`
- **withdraw-request-one-payout-consumed:**
  `4b5NvcGM3iow6o3Qc9ATpQLp9PlPv1Y62XvPTcbzUkQ=`,
  `9ncN7iBzDJswNbex9fiAbG37vXl9qu6HOrLPe16yp0w=`
- **immediate-withdraw-two-payout:**
  `jR18KgOd7Bmlhja1EmBRM_b-bPrdMuJwvsnfwhdYPiY=`,
  `BGhr3QvCA4-8NaUWkjqMUyEivmDuCF37sGpBS6W0x6w=`
- **op-0x2-double-payout:**
  `APpTI_e4-tKeBPZgtCDXmZhharEE-tvVh0tBPoCK6i0=`,
  `DAL_tL08iL0GO1ZqDkqd6kpl4NNmFurhegqjrKiW0sc=`
- **mixed-vesting:** `IG3rAERJtYBswEBCpy_3redfrE-4cQS0cAfg1-W1UO8=`

F4 (arity-1 immediate) and F7 (≥3 payouts) were hunted for but not found in 4
years of mainnet history; those shapes are covered instead by the synthetic
trees in `indexer/tests/mch/integration/test_nominator_withdraw.py`. The
`twins/py_actions.py` `PORTED_BTYPES` addition for `nominator_pool_deposit` was
required for the twins gate to pass with the new deposit fixtures.

Re-baselined 2026-07-31 (**824 → 838 rows, no fixture added**) by the COCOON
port — the whole twelve-matcher family in one new spec file, `specs/cocoon.mch`,
landing last in `ir/emit_manifest.json` exactly as the twelve legacy matchers sit
last in `event_processing.matchers`. Thirteen touched fixtures (the family has
100 % fixture coverage and zero live volume — see the port audit).

**Every row differs textually again**, for the same reason as the two
re-baselines below: TWELVE new composite columns (one per matcher) joined
`Classify.cpp render_action`, which prints the full column list on every row, so
all 824 pre-existing rows gained twelve `…=null` keys. Re-diffing with those
twelve keys stripped isolates the real delta: **14 rows added, 0 removed**, and
on pre-existing rows the ONLY fields that changed are `parent_action_id` and
`ancestor_type` — the 45 `call_contract` children the twelve new matchers now
consume. None of the twelve btypes is in `v1_ops`, so a consumed block is never
dropped, only re-parented (the tgbtc / layerzero / evaa_supply pattern).

**+14, not +12**: two matchers fire on two fixtures each — `cocoon_worker_payout`
on the regular (`1LzTIRFP…`) and last (`1gnDaOa5…`) payout shapes, and
`cocoon_client_request_refund` on the direct (`AMdf-I0N…`) and via-wallet
(`A88GSRUq…`) shapes. The other ten fire once each.

Beyond the golden, every one of the 14 rows is gated three ways: the actions
twin diff is 0 (C++ == the Python IR engine), the parity-sweep row
`specs/cocoon.mch` is green on all 13 fixtures (Python IR == the twelve legacy
`BlockMatcher`s, compared field by field), and `tests/test_cases/cocoon.yaml`
(13 cases, pre-existing) asserts the production Python classifier independently.

Two things about this wave are worth remembering.

**A 260-bit external address used to render `NaN`.** `load_address_py`
(`parse/PSlice.cpp`) built the addr_extern value into a `td::RefInt256`, which is
a signed 257-bit BigInt — so anything past 256 bits became an
invalid-but-non-null value. Cocoon is the first family to read an
`expectedMyAddress`, and the corpus ones are 260 bits. The Str payload is now
`extern:<len>:<HEX>` rather than `<decimal>`, assembled nibble by nibble off the
wire (exact to 511 bits) and twinned by Python's `f"{int:x}"`. Nothing else in
the engine reads an extern address, so the contract change is contained to
`PSlice.cpp`, `twins/py_msg_dump.py`, four doc comments and the one host fn that
wants it — which now just swaps the second colon for the semicolon legacy uses.

**`nonce` moved into `decimal_string_key`** (`emu/EmuActionSerialize.cpp`).
`cocoon_client_register`'s nonce is a raw u64 that routinely exceeds int64
(`11924145372215500834` on the `5B6Ex8kv…` fixture), so packing it as a natural
integer would have nil'd it out. The key table is flat by key NAME, so
LayerZero's bounded `nonce` — previously an explicit exclusion — now travels as
a decimal string too. Lossless: both Go fields are `*int64` and `decodeFlexInt`
parses the string back. The three affected msgpack self-test assertions were
flipped from `is_uint` to `is_str`.

Re-baselined 2026-07-31 (**823 → 824 rows, no fixture added**) by the W4
FIXTURED SINGLES port — three unrelated matchers, one fixture each, appended to
three existing spec files: `vesting_add_whitelist` (`specs/vesting.mch`),
`tonco_deploy_pool` (`specs/tonco_liquidity.mch`) and
`ethena_withdrawal_request` (`specs/ethena.mch`).

**Every row differs textually again**, for the same reason as the layerzero
re-baseline below: two new composite columns (`vesting_add_whitelist_data`,
`tonco_deploy_pool_data`) joined `Classify.cpp render_action`, which prints the
full column list on every row, so all 823 pre-existing rows gained two `…=null`
keys. Re-diffing with those two keys stripped isolates the real delta:
**24 rows added, 23 removed**, net +1.

- **tonco_deploy_pool, +5 top-level.** All five on `4Zs27oXg…`, the one corpus
  trace that deploys pools (five in one trace). The 15 calls they consume
  (5× `0x2e3034ef`, 5× `0x441c39ed`, 5× `0xd53276db`) come BACK as child rows —
  `tonco_deploy_pool` is not in `v1_ops` — so those 15 rows are removed and
  re-added carrying `parent_action_id` + `ancestor_type=[tonco_deploy_pool]`.
- **vesting_add_whitelist, +1 top-level.** `03XZQ1Ck…`; same pattern, its two
  consumed calls (`0x7258a69b` request, `0xf258a69b` response) re-parented.
- **ethena_withdrawal_request, +1 and −6.** `_KePnuNG…` gains one
  `stake_withdrawal_request` row and LOSES the six rows it consumed (five
  `call_contract` plus one `jetton_mint`). This is the opposite of the two above
  because `ethena_withdrawal_request` IS in `v1_ops`
  (`block_tree_serializer.py:1679`), so its consumed blocks are dropped rather
  than re-parented — exactly what the legacy classifier does on that trace,
  which produces that single action and nothing else.

Verified beyond the twin diff: all seven new rows were compared field-by-field
against the REAL legacy Python classifier (production `event_classifier
.process_trace`, i.e. `event_processing.matchers` + `trace_post_processors`
through the same serializer) — **7 rows, 161 field comparisons, 0 mismatches**,
matched BY `action_id`, so these are the same rows and not merely similar ones.
Every field also matches `tests/test_cases/vesting.yaml`, `tonco.yaml` and
`ethena.yaml`, which `tests/test_actions.py` asserts against the production
classifier independently. All three specs have parity-sweep rows and none needs
an `_AUX_ABSORB_SPECS` waiver.

`ethena_withdrawal_request` carries **`priority 85`**, the one ordering subtlety
in this wave: it anchors on the RAW `0x0f8a7ea5` call and binds the RAW
`0x642b7d07` mint, both of which the priority-90 jetton family would otherwise
turn into blocks first. The legacy list interleaves them (transfer 14, ethena 48,
mint 49), which one priority per spec file cannot express; 85 puts ethena ahead
of all three jetton matchers. Unobservable — see the spec header for the
construction argument (the matcher's guard 3 requires the internal transfer to
land on a jetton MASTER, which is precisely the shape
`build_jetton_transfer_core` rejects).

The whitelist's hand parser (`parse_vesting_add_whitelist`, the unbounded
one-address-per-cell ref chain) is deliberately NOT in the `--parse-dump` A/B
table: the dump renderer has no List form on either side, the same reason
`JVaultClaim`'s `dict_keys267` list is absent from it. Its gate is this golden
plus the parity-sweep row, which compares `accounts_added` element by element.

Re-baselined 2026-07-31 (**816 → 823 rows, no fixture added**) by the LAYERZERO
COMPLETION port — four matchers appended to `specs/layerzero.mch`, joining the
already-shipped `layerzero_receive`: `layerzero_send`, `layerzero_send_tokens`,
`layerzero_commit_packet` and `layerzero_dvn_verify`. Six touched fixtures.

**EVERY row differs textually, and almost none of that is a content change.**
Two new composite columns (`layerzero_send_data`, `layerzero_dvn_verify_data`)
joined `Classify.cpp render_action`, which prints the full column list on every
row, so all 816 pre-existing rows gained two `…=null` keys. Re-diffing with those
two keys stripped isolates the real delta: **7 rows added, 0 removed, and on
pre-existing rows the ONLY fields that changed are `parent_action_id` and
`ancestor_type`** — the 60 child rows (42 `call_contract`, 17 `ton_transfer`,
1 `jetton_transfer`) the four new matchers now consume. Same pattern as
`evaa_supply` and tgbtc: none of the four new btypes is in `v1_ops`, so a
consumed block is never dropped, only re-parented.

**+7 rows, all added, all parents except one.**

- **send, +2 top-level.** `01yfeTlz…` (msg to Tron, request 90) and
  `0_AQj88L…` (TON→TON self-send, request 5).
- **send_tokens, +1 parent AND +1 child.** `KEbatIe0…` gains the
  `layerzero_send_tokens` row plus a `layerzero_send` CHILD row: the tokens
  matcher consumes the send block, `layerzero_send_tokens` is not in `v1_ops`,
  so the consumed product comes back as a child carrying `parent_action_id` +
  `ancestor_type=[layerzero_send_tokens]` — the `evaa_supply` pattern.
- **commit_packet, +2.** `0vFe6hsN…` (TON→TON, nonce 1) and `12Jtky5t…`
  (ETH→TON, nonce 194).
- **dvn_verify, +1.** `0IpCwGaM…`, status `succeeded`, nonce 125.

Verified beyond the twin diff: all seven fixtures were rendered through the REAL
legacy Python classifier (production `event_processing.matchers` +
post-process passes, through the same serializer) and compared row-by-row —
**81 rows, 3645 field comparisons, 0 mismatches**, action_ids included, so the
C++ rows are the same rows and not merely similar ones. Every field also matches
`tests/test_cases/layerzero.yaml`, which `tests/test_actions.py` asserts against
the production classifier independently. The spec has a parity-sweep row running
all 184 fixtures and needs no `_AUX_ABSORB_SPECS` waiver, so it carries no §4.5
aux-absorption deviation.

The three new hand parsers (`LayerzeroChannelSendCallback`,
`ChannelCommitPacket`, `UlnConnectionVerifyCallbackParser`) were added to the
`--parse-dump` A/B table as well, so they are gated field-for-field against
their Python classes over the whole corpus (diff 0, 714 lines, 6 new rows).

Re-baselined 2026-07-31 (**808 → 816 rows, no fixture added**) by the TGBTC
COMPLETION port — five matchers appended to `specs/tgbtc.mch`, joining the
already-shipped `tgbtc_mint`: `tgbtc_burn`, `tgbtc_new_key`, and the four
log-only fallbacks (`tgbtc_mint_log_only`, `tgbtc_burn_log_only`,
`tgbtc_new_key_log_only`, `tgbtc_dkg_log_only`). Eight touched fixtures, no
new column, no new composite.

**Header note:** the "184 fixtures, N ACTION rows" line above had TWO
conflicting values (872 and 795) — a merge collision between the coffee-tail and
dedust-v2-deposit waves, each of which landed its own headline count. Collapsed
to the one measured value.

**+8 rows, every one of them an ADDED parent row.** None of `tgbtc_burn` /
`tgbtc_new_key` / `tgbtc_dkg_log` is in `v1_ops`, so the pattern is the
`evaa_supply` one below: the protocol row is added and the blocks it consumed
STAY as CHILD rows carrying `parent_action_id` + `ancestor_type=[<the new
btype>]`. No row is ever removed; the 26 rows that differ textually without
being added are exactly those child rows gaining their two new fields
(18 `call_contract`, 4 `jetton_burn`, 4 `ton_transfer`).

- **burn, +5 over five fixtures.** `xBl_8BTU…`, `Ez03fWgL…`, `TDsLHsBM…`
  (the "strange notification" `0x587643A2` head) and `2yH8U5KF…` (the "weird
  schema" body — the one that pins the `min`/zero-fallback amount ternary, yaml:
  "amount: 93066 # from burn msg") each gain one `tgbtc_burn`; `35MtsOa0…` gains
  one `tgbtc_burn_fallback` from the log-only matcher, its truncated log body
  yielding addr_none for both addresses.
- **new key, +2 over two fixtures.** `XbaXJMw3…` and `SSUQR8t_…`.
- **dkg log, +1 on one fixture.** `_6GHAnnp8…` — a dkg log the new-key matcher
  could not claim (the `0x690F357A` request is present but emits no new-key
  log), so `tgbtc_new_key` rejects and the fallback takes the leftover log.

Verified beyond the twin diff: all 8 rows were compared field-by-field against
the REAL legacy Python classifier (production `event_processing.matchers` +
`trace_post_processors`, rendered through the same serializer) — 72 field
comparisons over 8 rows, 0 mismatches, script `legacy_verify_tgbtc.py`. The
action_ids match too, so the C++ rows are the same rows, not merely similar
ones. The spec has a parity-sweep row (running on all 184 fixtures, since
`tgbtc_burn` is btype-anchored) and needs no `_AUX_ABSORB_SPECS` waiver, so it
carries no §4.5 aux-absorption deviation.

TWO of the six matchers have NO fixture of their own: `tgbtc_mint_log_only` and
`tgbtc_new_key_log_only` would need a trace where the mint / new-key chain is
absent but its log survives. They ride the six mint/new_key fixtures as
REJECT-identical sweep rows (both legs must leave the log consumed by the full
matcher and emit no `_fallback` block) and share their fill with a fixtured
sibling. Their POSITIVE path is pinned instead by a synthetic legacy-vs-IR test,
`indexer/tests/mch/integration/test_tgbtc_log_only.py` (real
Transaction/Message models carrying a genuine event BOC, the test_dns.py
pattern) — so the only thing a real fixture would add is C++ coverage of the
`tgbtc_mint_log` parse fn, the one host fn in the family no corpus trace
reaches (`tgbtc_new_key_log` is covered via the full new_key matcher).

Re-baselined 2026-07-31 (**859 → 872 rows, no fixture added**) by the COFFEE
TAIL port — six matchers over three new specs: `specs/coffee_create_pool.mch`
(`coffee_create_pool_creator` + `coffee_create_pool`), `specs/coffee_mev.mch`
(`coffee_mev_protect_hold_funds`, split into a TON arm and a jetton arm) and
`specs/coffee_staking.mch` (`coffee_staking_deposit` / `_withdraw` /
`_claim_rewards`). Nine touched fixtures, no overlap between the three specs,
and each was checked field-by-field against the REAL legacy Python classifier
(`event_processing.matchers` + `serialize_blocks`) — 188 field comparisons over
13 rows, all equal, script `legacy_verify_coffee_tail.py`. All three specs have
a parity-sweep row and none needs an `_AUX_ABSORB_SPECS` waiver, so none carries
the §4.5 aux-absorption deviation.

Note the whole file also gains THREE columns — `coffee_create_pool_data`,
`coffee_staking_deposit_data`, `coffee_staking_withdraw_data`, the composites
these fills add to the Action struct — so EVERY row differs textually even where
nothing about it changed.

**+13 rows, and every one of them is an ADDED parent row.** None of the six
btypes is in `v1_ops`, so the pattern is the `evaa_supply` one below: the
protocol row is added and the blocks it consumed STAY as CHILD rows carrying
`parent_action_id` + `ancestor_type=[<the new btype>]`. No row is ever removed;
each fixture grows by exactly the number of coffee rows it produces.

- **create pool, +5 over three fixtures.** `1TUCte7d…` 6 → 7 (a single jetton
  leg, so one `coffee_create_pool_creator` and no pool yet), `1VcQkyvd…` 15 → 18
  and `3uecEX5T…` 18 → 21 (both legs land: two creators plus the
  `coffee_create_pool` that consumes the second one).
- **mev protect, +1 on one fixture.** `11WhhXiP…` 2 → 3 — the TON arm; the
  `#6bc79e7e` call and the `#c0ffeea0` service fee become its children.
- **staking, +7 over five fixtures.** `K_PlZB5R…` and `00wjHu7i…` 4 → 5
  (deposit), `013-itRD…` and `0HIS6vA7…` 2 → 3 (claim rewards), `T-oXSZb9…`
  4 → 5 (withdraw).
Re-baselined 2026-07-31 (**859 → 795 rows, no fixture added**) by the DEDUST V2
DEPOSIT port (`specs/dedust_v2_deposit.mch` — the final / rejected / partial
trio) together with its Phase-E pass `post_process_dedust_v2_liquidity`
(`ClassifyCore.cpp`, now called at all three post-process sites). One delta over
nine fixtures.

**−73 loose rows, +9 composite rows.** The removed rows are 58 `call_contract`
and 15 `jetton_transfer` leaves; both produced btypes ARE in `v1_ops`, so no
CHILD rows are expanded and the consumed leaves disappear entirely. Eight
fixtures collapse to a single `dedust_v2_deposit_liquidity` and the ninth
(`a4iGagV3…`) to a single `dedust_v2_deposit_liquidity_partial` — the one trace
whose completing leg the matcher does not claim, which is what keeps the partial
row path covered.

The Phase-E pass is why the count is 9 and not 17: a deposit's two asset legs
are matched separately (only the last-credited one reaches the JoinLiquidity
anchor through the parent chain), and the pass folds the partial into the
completing block, keyed on the Deposit escrow address, before serialization.

Verified beyond the twin diff: for all nine fixtures the produced
`dex_deposit_liquidity` rows were compared field-by-field against the REAL
legacy Python classifier (production `event_processing.matchers` +
`trace_post_processors`, rendered through the same serializer) — 0 mismatches,
one row per fixture on both sides. The parity sweep compares the consumed
event-node multisets with **no** `_AUX_ABSORB_SPECS` waiver, so unlike the
sibling withdraw spec this family carries no §4.5 aux-absorption deviation.

No new composite column: both fills reuse `dex_deposit_liquidity_data`, so rows
outside the nine touched fixtures are textually unchanged.

Previously re-baselined 2026-07-31 (**872 → 859 rows, no fixture added**) by the TONCO
WITHDRAW port (`specs/tonco_liquidity.mch`, second matcher) and the EVAA SUPPLY
port (`specs/evaa.mch`, second matcher). Two independent deltas over six
fixtures, no overlap, and every touched fixture was checked field-by-field
against the REAL legacy Python classifier (`event_processing.matchers` +
`serialize_blocks`): every semantic field of every produced row is equal, and
the parity sweep compares the consumed event-node multisets with no
`_AUX_ABSORB_SPECS` waiver for either spec, so neither carries the §4.5
aux-absorption deviation.

Note the whole file also gains one column, `evaa_supply_data` — the composite the
supply port adds to the Action struct — so EVERY row differs textually even where
nothing about it changed.

- **Tonco withdraw, −16 rows over three fixtures, each collapsing to one row.**
  The `#530b5f2c` / `#46ca335a` / `#d73ac09d` / `#a1daa96d` chain plus its payout
  legs becomes one `tonco_withdraw_liquidity`, which the fill rewrites to
  `dex_withdraw_liquidity`. That btype IS in `v1_ops`, so no CHILD rows: the
  consumed leaves disappear entirely.
  - `0JSRYHwE…` 7 → 1 (TON + jetton legs; the TON leg's pTON call is consumed too),
    `0Jzh0p9L…` 6 → 1 (one real jetton leg, one router-fallback leg),
    `1XI-Ow6Y…` 6 → 1 (two real jetton legs).
- **EVAA supply, +3 rows over three fixtures.** `evaa_supply` is NOT in `v1_ops`,
  so — exactly like the `evaa_withdraw` rows already in this file — the parent row
  is ADDED while its consumed leaves stay as CHILD rows carrying
  `ancestor_type=[evaa_supply]`. No existing row is removed.
  - `Pz4XQKMB…` 5 → 6 (TON supply: `#1` master call anchor),
    `j-5yrT2H…` 4 → 5 and `ww5gZmgo…` 4 → 5 (jetton supply, success and failed:
    the anchor is the jetton transfer whose forward payload is the `#1` master
    message, which only the `evaa_supply_anchor` host predicate can see).

Previously re-baselined 2026-07-31 (**906 → 872 rows, no fixture added**) by the DEDUST V2
WITHDRAW port (`specs/dedust_v2_withdraw.mch`) and the ELECTOR port
(`specs/elections.mch`). Two independent deltas over nine fixtures, no overlap,
and every touched fixture was checked row-by-row against the REAL legacy Python
classifier (`twins/py_actions.py` driven with `event_processing.matchers`).

- **DeDust V2 withdraw, −33 rows over eight fixtures.** Each collapses its
  `#20b5ef89` / `#bc1531a9` / `#f6f6a3aa` chain — plus the payout legs, where
  there are any — into one `dex_withdraw_liquidity`. `dex_withdraw_liquidity` is
  in `v1_ops`, so no CHILD rows anywhere in this delta.
  - Three ABORTED withdraws (no Withdrawal event, hence `is_refund=true` and the
    burnt amount off the entry message): `-mNf-yQ8…`, `T4FDfsNC…`, `gr3f68A6…`,
    5 → 2 each (the surviving row is the trace's unrelated second action).
    **Byte-identical to legacy.**
  - Five SUCCESSFUL withdraws: `1Q2c26rR…` and `phSM_Wdd…` (two jetton legs),
    `GsBbieLd…` and `OShVZJVq…` (jetton + TON), `XoYtOPQk…` (TON + jetton).
    Every semantic field matches legacy; the ONE difference is that the pool's
    `0xd53276db` excess is absorbed into the row (its tx hash joins `tx_hashes`,
    and on `phSM_Wdd…` it extends `end_lt`) instead of surviving as a standalone
    `call_contract`. That is §4.5 nested aux absorption, which is unconditional
    for a matched non-root node on BOTH engines while the reference turns it off
    per nested matcher — documented at length in the spec header, not fixed
    (honouring the flag would move every other include_excess=false spec).
  - `7rGTg9yc…` is UNCHANGED and correct: its Position never answers with
    ExitLiquidity, so the chain is truncated and neither engine classifies it —
    legacy leaves the same three loose `call_contract` rows.
- **Elector recover, −1 row on one fixture.** `a0rjbO1X…` 11 → 10: the
  `#47657424` request and its `#f96f7324` confirmation become one
  `election_recover` (`amount` off the confirmation's value). **Byte-identical
  to legacy**, including its three `stake_withdrawal` neighbours from the
  previous wave. `election_deposit` has ZERO corpus coverage — no fixture
  carries `#4e73744b` — so that half of the port is unexercised here.

Previously re-baselined 2026-07-31 (**902 → 906 rows, 181 → 184 fixtures**) by three live
tonstakers DEPOSIT fixtures fetched off the cycle-10 A/B window. **No existing
row changed** — the whole delta is the three new fixtures, and every one of them
already classified correctly against the spec as committed the day before (the
cycle-10 "stake_deposit missed ×3" class is NOT reproducible from these traces).
They are kept as regression coverage because they are the only deposit traces in
the corpus besides `rbzKlyNg…`.

- `fyrdkfqB…` (+1) and `rU8BN41C…` (+1): one `stake_deposit` each.
- `7Rgomgm…` (+2): `stake_deposit` plus a `contract_deploy#178d4519` — the only
  corpus deposit where the internal transfer DEPLOYS the user's tsTON wallet.
- All three are structurally identical to `rbzKlyNg…`: DepositRequest
  0x47d54391 one hop under an external-in wallet op, then MintJettons ->
  InternalTransfer -> Notify + excess. They differ only in the driving wallet
  opcode (0x9fb53633 / 0x7369676e / 0x339d6469), which no matcher looks at.
- All 4 rows verified byte-identical against the REAL legacy Python classifier,
  and identical across the fixture, `--actions-schema` and `--actions-prod`
  loader paths.

Previously re-baselined 2026-07-30 (**911 → 902 rows, no fixture added**) by the TONSTAKERS
family completion (`specs/tonstakers.mch`) and the DEDUST V2 CLAIM_FEES port
(`specs/dedust_v2_claim_fees.mch`). Two independent deltas, no overlap, and every
touched row was checked against the REAL legacy Python classifier
(`twins/py_actions.py` driven with `event_processing.matchers`) — all 48 rows on
the nine affected fixtures come out byte-identical.

- **Tonstakers, −14 rows over four fixtures.** The spec gained
  `tonstakers_deposit` and `tonstakers_delayed_withdrawal`, and all three of its
  matchers gained `priority 85`.
  - The **priority** is the bigger half. `tonstakers_withdraw` anchors on
    JettonBurn 0x595f07bc, which is also the anchor of the priority-90
    `jetton_burn` in `specs_decl/jettons.mch`. Legacy registers
    TONStakersWithdrawMatcher (idx 5) well ahead of JettonBurnBlockMatcher (idx
    17); the artifact had it at 100, so the jetton family ate the anchor and the
    entire withdraw family was suppressed. `GO-NI0Ej…` 6 → 4 (a `jetton_burn`
    plus two `call_contract` collapse into one `stake_withdrawal_request`, ts_nft
    `0:7ABD…3025`; its `nft_mint` / `nft_transfer` / `contract_deploy` siblings
    are untouched) and `oND3Z5gM…` 3 → 1 (`jetton_burn` + `#319b0cdc` +
    `#0a77535c` → one `stake_withdrawal`). Neither the parity sweep nor any
    per-protocol test could see this: both run one spec's matchers in isolation,
    never the registered set against each other.
  - `rbzKlyNg…` 6 → 2: five `call_contract` (`#47d54391`, `#1674b0a0`,
    `#178d4519`, `#7362d09c`, and the `#d53276db` excess the internal-transfer
    node absorbs) become one `stake_deposit`; the `contract_deploy#178d4519` row
    the deployment unwind adds survives.
  - `a0rjbO1X…` 17 → 11: three independent NFT-burn payout chains
    (`#f127fe4e` → `#ed58b0b2` → `#db3b8abd`) each collapse into a
    `stake_withdrawal` with `burnt_nft` set and `asset` null. The two elector
    rows on that fixture (`#47657424`, `#f96f7324`) stay loose — ElectionRecover
    is still unported, unrelated to this change.
  - One real bug fell out and is fixed on both engines: `tonstakers_minted_nft`
    looked for its minted NFT with `isinstance(NftMintBlock)` plus a
    TONStakersInitNFT `call_contract` fallback, and BOTH arms were dead in
    production once `specs/nft_mint.mch` (priority 80) started consuming the
    InitNFT call into a generic `nft_mint` block. It now probes the btype. The
    sweep hid it because its prerequisite is the LEGACY NftMintBlockMatcher,
    whose product does satisfy an isinstance.
  - No CHILD rows anywhere in this delta: all three tonstakers btypes are in
    `v1_ops`.
- **DeDust V2 claim_fees, +5 rows over five fixtures, 25 rows rewritten.** The
  five `call_contract`/`jetton_transfer` legs of each claim keep their rows but
  become CHILD rows (`dedust_v2_claim_fees` is outside `v1_ops`, same as
  `dedust_v2_claim_reward`), and each fixture gains one top-level
  `dedust_v2_claim_fees`. Fixtures: `846TnKmL…` (single TON leg matching y_fees,
  x empty), `t3w52NsD…` (TON + jetton), `fjQUb23k…` and `cymBOUsf…` (two jetton
  legs), `tJchcmap…` (both fees zero, no payout leg at all).

Previously re-baselined 2026-07-30 (**907 → 911 rows, 180 → 181 fixtures**) by the TELEGRAM
NFT parent-edge widening (`specs/telegram_nft.mch`). The spec's documented scope
cut — ton_transfer-funded purchases only — left three legacy shapes
unclassified; two already had corpus coverage nobody had claimed, and the third
was recorded for this change.

- **Rows: −2 on two existing fixtures, +6 from one new fixture. No other row
  touched.**
- NEW FIXTURE `rV0MIZB9AKrWu2IUpsDnGjiL4SufrJ-g5xbCZFq5KuE=` (+6 rows), the
  telemint MINT variant and the largest live A/B diff class: the NFT item is
  deployed by a 0x299a3e15 call and the assignment carries a teleitem bid
  payload, but the parent is a mint, so `is_purchase` is FALSE, `price` is null
  (the serializer gates it on `is_purchase`), `marketplace` is `fragment`, and
  the two `AuctionFillUp` payouts stay their own `call_contract` rows because
  this branch of legacy absorbs nothing but the anchor. Rows:
  `nft_transfer` + `nft_mint` + `contract_deploy#299a3e15` +
  2×`call_contract#370fec51` + `call_contract#4637289b` — all six byte-identical
  to the legacy matcher.
- Existing fixtures, **−2**:
  - `5b-KXr82…` 4 → 2: the external-driven Fragment purchase (an ext-in message
    drives the NFT item directly, which emits the assignment plus two
    `AuctionFillUp` payouts). Three `call_contract` rows (`#05138d91` +
    2×`#370fec51`) collapse into ONE `nft_transfer` (`is_purchase=true`,
    `marketplace=fragment`, `price=10000000000`, `payout_amount=8496523073`,
    `royalty_amount=499933333`) — the new
    `btype call_contract where (.source == null)` parent branch of
    `telegram_nft_purchase`.
  - `GO-NI0Ej…` same count: one `call_contract#05138d91` becomes `nft_transfer`
    (`is_purchase=false`, `marketplace=null`, `price=null`) — an orphan
    assignment with no forward payload, claimed by the new non-consuming
    `telegram_nft_assigned` matcher. Its `nft_mint` sibling row survives, which
    is the whole reason that matcher consumes the anchor alone.
- All three deltas were checked row-for-row against the legacy
  `TelegramNftPurchaseBlockMatcher` driven through the same loader and
  serializer (`twins/py_actions.py` with `event_processing.matchers`): the
  produced rows are IDENTICAL.
- Still uncovered by the corpus: a single-payout fragment purchase (the
  `len(payouts) == 1` core branch).

Previously re-baselined 2026-07-30 (**894 → 907 rows, 176 → 180 fixtures**) by the GETGEMS
SALE/AUCTION FAMILY port (`specs/nft_sale.mch`). Four fixtures were captured
from the live A/B windows specifically because the family had ZERO corpus
coverage — every matcher in the spec fired on nothing before this.

- **Rows**: **+13, none removed, and not one existing row changed** — the whole
  delta is the four new fixtures. By fixture: `M2rUrfJX…` 5
  (`nft_put_on_auction` + 2 `nft_transfer` + `ton_transfer` + `contract_deploy`),
  `EzvOGPJe…` 3 (`nft_cancel_sale` + `nft_transfer` + `call_contract`),
  `LQB6Qv2a…` 3 (`nft_cancel_auction` + `nft_transfer` + `ton_transfer`),
  `_4VnZDY…` 2 (`nft_update_sale` + `call_contract`).
- **6 of the 13 are CHILD rows** (7 top-level). None of these four btypes is in
  `v1_ops`, so the P3 recursion re-emits each matcher's merged legs with a
  `parent_action_id` — the first non-teleitem exercise of that path in the
  corpus. The returned `nft_transfer` of the two cancel matchers is deliberately
  NOT among them: legacy gates on that child but merges only the anchor, so it
  stays a TOP-LEVEL row, and `specs/nft_sale.mch` reaches it through
  `nft_trade_returned` rather than a pattern edge to reproduce that.
- `M2rUrfJX…` is the only fixture that exercises `init_state`: its
  `nft_listing_data` (mp_fee_factor 2, mp_fee_base 100, royalty_fee_base 100,
  max_bid 0, min_bid 30000000000) is read out of the deploy message's GetGems
  StateInit by `getgems_auction_init`, and matches the legacy Python
  `get_auction_data` field for field. Its `NftAuction.code_hash` is null (no
  detector Result carries one), so both engines take the "latest" → `v3r3`
  branch; the other six version branches remain unexercised offline.
- All 13 rows were checked field-for-field against the REAL legacy Python
  classifier (`process_trace`) — type, accounts, tx_hashes, start/end_lt,
  `parent_action_id` and `ancestor_type` all identical. That comparison is worth
  spelling out because neither standing gate makes it: the twin runs the IR
  engine on BOTH legs, and the parity sweep compares blocks, not action rows.

Re-baselined 2026-07-30 (**712 → 894 rows**, and EVERY row gains two columns) by
the CHILD-ACTION RECURSION port (`serialize_blocks`,
`block_tree_serializer.py:1645-1681`). Two effects:

- **Columns**: `parent_action_id` and `ancestor_type` join the rendered column
  set (`Classify.cpp` `render_action` + the twin's `render_action`), so all 894
  lines carry `parent_action_id=null,ancestor_type=[]` unless they are child
  rows. Whole-file churn.
- **Rows**: **+182**, none removed, over **32 fixtures**. A spine block whose
  btype is NOT in `v1_ops` now re-emits the legs a matcher merged into it as
  rows of their own, linked back by `parent_action_id`. By parent btype:
  `tgbtc_mint` 40, `jvault_stake` 35, `evaa_withdraw` 26,
  `jvault_unstake_request` 20, `layerzero_receive` 18,
  `dedust_v2_claim_reward` 15, `jvault_unstake` 12, `jvault_claim` 5,
  `vesting_send_message` 4, `coffee_create_vault` 4,
  `teleitem_cancel_auction` 2, `teleitem_start_auction` 1. By child btype:
  160 `call_contract`, 11 `jetton_transfer`, 7 `ton_transfer`, 4 `jetton_mint`.
  No corpus fixture nests two levels, so every `ancestor_type` here has exactly
  one element; the recursion itself is unbounded.
  Spot-verified against the REAL legacy Python classifier (`event_processing
  .matchers` + `serialize_blocks`) on three fixtures — teleitem
  (`4AAiT_8j…`), jvault_stake (`-OII6CIr…`), tgbtc_mint (`1OrxQbhi…`) — row
  sets, action_ids, parent links and ancestor lists all identical, including
  the `contract_deploy` children that stay TOP-LEVEL rows.

Re-baselined 2026-07-30 (**745 → 731 rows**, and EVERY row gains one column) by
the `nft_mint` port (`specs/nft_mint.mch`, the mch counterpart of
`NftMintBlockMatcher`). Two independent effects:

- **Column**: `nft_mint_data` joins the rendered column set (`Classify.cpp`
  `render_action` + the twin's `ACTION_COLUMNS`), so all 731 lines carry a new
  `nft_mint_data=null` between `nft_listing_data` and `opcode` — except the three
  `nft_mint` rows, which carry `{nft_item_index=…}`. This is the only whole-file
  churn in the diff.
- **Rows**, over exactly 6 fixtures (`-13 call_contract`, `-4 jetton_transfer`,
  `+3 nft_mint`, 3 `tonco_deposit_liquidity` rows rewritten):
  - three fixtures (`00wjHu7i…`, `GO-NI0Ej…`, `K_PlZB5R…`) swap the deploying
    `call_contract` leaf for the `nft_mint` row it now builds;
  - three tonco fixtures (`0--IKFmIb…`, `0GW2kNF4…`, `0_JGy9zy…`) lose the legs
    that `tonco_deposit_liquidity` can finally absorb — that matcher binds
    `btype nft_mint`, so until this port it was shipped-but-dead and emitted a
    row with `nft_address` / `nft_index` / `lp_tokens_minted` all null. Those
    three rows are now filled (and their tx_hashes/lt ranges widened).

Re-baselined 2026-07-30 (**765 → 746 rows**) by the `dedust_v2` swap port
(`specs_decl/dedust_v2_swap.mch`, the mch counterpart of
`DedustV2SwapBlockMatcher` + `DedustV2FailedSwapBlockMatcher`). DeDust CPMM V2
swaps were entirely unclassified: every one of them serialized as its raw legs.
10 fixtures changed:

- **-14 `call_contract`, -15 `jetton_transfer`, +10 `jetton_swap`** — seven
  successful swaps (three of them multi-hop, with `peer_swaps` filled from the
  per-hop Swap events) and three slippage refunds, each absorbing the leg rows
  that used to stand in for it.
- **9 of the 10 became byte-identical to the LEGACY Python classifier**
  (`event_processing.matchers` driven through the twin's renderer), including the
  `(ab-gap)` fixture below. The tenth (`MZQz_qHLV8Oi…`) matches legacy on its
  dedust_v2 row and still differs on an unrelated **dedust V1** swap row in the
  same trace, whose leg wallets the C++ host leaves null — the same pre-existing
  gap the pton re-baseline recorded, byte-identical to the previous golden.

Re-baselined 2026-07-30 (**818 → 765 rows**) by the `pton_transfer` matcher port
(`specs_decl/pton_transfer.mch`, the mch counterpart of `PTonTransferMatcher`).
pTON wallets speak the jetton transfer interface but emit no internal transfer,
so until this port every pTON leg stayed an unconsumed `call_contract` and the
DEX matchers that bind `btype jetton_transfer` lost their TON side. 15 fixtures
changed:

- **-58 `call_contract`, +10 `jetton_swap`, -5 `jetton_transfer`** — the swaps
  those legs unblocked (stonfi v1 and v2), each absorbing the call rows that used
  to stand in for them. The 5 lost `jetton_transfer` rows are pton products a
  swap now consumes; the surviving ones are pton legs no matcher claims, which
  Python emits as their own rows too.
- **11 of the 15 became byte-identical to the LEGACY Python classifier**
  (`event_processing.matchers` driven through the twin's renderer), including both
  `(ab-gap)` fixtures below. The other four already diverged from legacy before
  this change, for reasons it does not touch (matchers absent from the spec set;
  a dedust swap whose leg wallets the C++ host leaves null) — their diverging rows
  are byte-identical to the previous golden.

Re-baselined 2026-07-30 (**277 → 787 rows**) when the row set was moved from the
matcher FIRE list to the `serialize_blocks` SPINE walk, which is what Python
actually does (`block_tree_serializer.py:1641-1681`). Per-row account of the
change, over 143 of the 165 fixtures:

- **+497 `call_contract`, +82 `contract_deploy`, +38 `ton_transfer`** — the basic
  rows for every leaf no matcher consumed. Python has always emitted these; the
  C++ dump only ever rendered matcher products.
- **-58 `jetton_transfer`, -5 `jetton_mint`** — products a LATER matcher absorbed.
  They are off the spine, so Python emits no row of their own for them (the row
  belongs to the outer composite); the fire list still had them.
- **-44 `unknown`** (50 → 6) — those fixtures were never rowless, only
  product-less. `unknown` now fires on the same condition Python uses: the
  serialized row set is empty.
- Every other row is unchanged in content. The `anchor=` label moved from the
  fire's anchor block to the row's own block for matcher products (a row has no
  fire to name), so composite lines differ in that prefix only.

Verified at each re-baseline (the 165-fixture figures are from the spine-walk one):

- `--actions` vs `twins/py_actions.py` diffed **0 lines** over all 165 fixtures
  (**176** at the pton and dedust_v2 re-baselines),
- `--actions-schema` and `--actions-prod` were **byte-identical** to `--actions`
  (the cross-check that catches a `fill_trace_aggregates` regression).

Row ORDER is sorted on both sides deliberately: Python's `compact_connections`
rebuilds `next_blocks` through a `set()`, so its spine order varies run to run,
and nothing downstream of the serializer depends on row order (they are DB rows
keyed by `action_id`).

## The 11 A/B-gap fixtures (added 2026-07-30, +31 rows)

Recorded from finalized mainchain blocks 82925391-82931345 with
`ton-fixture-save`, one per divergence class found in the live emulator A/B run
(C++ in-process classifier vs Python `event_classifier`). Python's rows are the
parity target and are asserted by pytest cases named `(ab-gap)` in
`indexer/tests/test_cases/`. **Three are FIXED**, three still carry a genuine C++
gap — those three golden rows are KNOWN-WRONG today and a matcher port should flip
them:

- ~~`mg8sRwic_zM8iy-pZoVdocmQsozeVKNt7G5j_k6nYyc=` pton stonfi v2 swap~~ — **FIXED**
  by `specs_decl/pton_transfer.mch`: 6x`call_contract` -> 1 `jetton_swap`, byte-identical to Python.
- ~~`2xripwtdxwKPdHABREASj6enBvKBTwsOVJx6YEZfI64=` pton stonfi v1 swap~~ — **FIXED**
  by the same port: 5x`call_contract`+2x`jetton_transfer` -> `call_contract`+`jetton_swap`+`jetton_transfer`, byte-identical to Python.
- ~~`2UJH7bBZDI4vmFG7O7HgraaQQF0BfjS2Eg5JaY62_Zo=` dedust v2 swap~~ — **FIXED**
  by `specs_decl/dedust_v2_swap.mch`: 2x`call_contract`+`jetton_transfer` -> `call_contract`+`jetton_swap`, byte-identical to Python.
- ~~`4AAiT_8jTvxhujXOywe0zZv1sylSlAwnZV9stEG3gnM=` teleitem start auction~~ — FIXED by specs/teleitem_auction.mch: call_contract -> teleitem_start_auction, byte-identical to Python
- ~~`rV1gzQRFXipz6XEByCTq_3Q2LZpa4i61aLsvHf1CR1A=` teleitem cancel auction~~ — FIXED by specs/teleitem_auction.mch: 2x call_contract -> teleitem_cancel_auction, byte-identical to Python
- `5b-KXr82wUSLIHPSHkFaW791xTyN0Rmu5h0mFm_4IdQ=` nft transfer — 3x`call_contract`, want 1 `nft_transfer`

The other five **already match Python here**, so their golden rows are correct and
they are regression guards, not pending work. Note what that means: the class
they were recorded for does NOT reproduce in this harness.

- `o6-kGFnpDMI4nMlMH1zrNNijL0taVyegbi2KlWkgJ-8=` stonfi v2, `PdxEcev5rXzXI-fHb1VPfFA1eLcm3n4Mx6NbsuATc3A=` tonco,
  `YBQYgsltVHCigC9q03KhXnBgho2bsxFuvOD8u6UC_J4=` dedust v1 failed —
  recorded as **host rejects**. All three emit the right `jetton_swap` off a
  fixture. The live rejection came from the host lookup tier (celldb/redis
  interface fetch), which `FixtureLookupSource` serves from the fixture instead,
  so the fixture cannot reproduce it. Chasing those needs a host-layer test, not
  this golden. A fourth host reject (stonfi v1, `Lo6cMzpA…`) was recorded and then
  deleted: only the host knows the intended out-asset, which sits on a pool-side
  wallet the trace never touches, so the fixture baked in a wrong `asset2` and the
  case asserted it. The class stays fixture-untestable until the format carries
  raw account state (fixture-v2).
- `TXFkYzpo-8IZvMcy8lhsrKG9QJH73JaYAwsAitgNqdM=` (4x`call_contract`) and
  `sS0j8bD7JQj7389dsGXLdNUisE6I1ApyvdPM1gkODVc=` (`call_contract`+`ton_transfer`) —
  recorded because the live A side emitted FEWER basic rows than B. That is the
  shortfall 362ff33 fixed by moving the row set to the `serialize_blocks` spine
  walk; the A/B snapshot predates it. Kept as guards for that fix.

## Using it

```
ton-mch-engine --actions -T indexer/tests/traces \
  | diff - ton-index-worker/ton-mch-engine/goldens/actions.golden
```

A diff is a **regression**, not a re-baseline. Update this file only through
review, with the reason for every changed row stated in the commit.
