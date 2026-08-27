---
module: HostNominator
source: ton-index-worker/ton-mch-engine/src/host/HostNominator.cpp
source-commit: 06d4c50
---

# HostNominator

Host bindings for nominator-pool withdrawal requests. One shared classifier collects a request's ton_transfer children as the payout set only when exactly one or two exist, ordered by min_lt; the nominator_withdraw_payout_amount host fn derives the emitted amount and the nominator_withdraw_absorb_payouts shaper merges exactly the same classified set into the produced block, so the recorded amount always corresponds to the absorbed legs. Withdrawals with zero or with three or more qualifying children consume no payouts. The shaper runs after the wrapper merge has turned the payouts into siblings of the produced block and temporarily relinks them into the connected chain that merge_blocks requires, restoring the reference links afterwards.

## classified_withdraw_payouts

`std::vector<Block *> classified_withdraw_payouts(const Block * request)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostNominator.cpp:19`
- callers: mch::nominator_withdraw_payout_amount (ton-index-worker/ton-mch-engine/src/host/HostNominator.cpp:43); mch::nominator_withdraw_absorb_payouts (ton-index-worker/ton-mch-engine/src/host/HostNominator.cpp:62)

Collects a withdraw request's ton_transfer children, keeping them only when exactly one or two exist.

Children are stable-sorted by min_lt so payout order is deterministic. Any other count clears the result, so arity zero and three or more consume no payouts.

## nominator_withdraw_payout_amount

`EvalResult nominator_withdraw_payout_amount(BuildEnv &, const std::vector<Value> & args)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostNominator.cpp:41`
- callers: none found in parsed TU set
- calls: `mch::Value::Value`, `mch::Value::null`, `mch::as_block`, `mch::classified_withdraw_payouts`, `mch::data_field`, `mch::rt_ok`

Emits the canonical payout amount for a classified withdraw request, Null when unclassifiable.

With two payouts the first child's value field is returned regardless of the requested amount. With one, the payout is returned only when both value fields are numeric and the payout exceeds the request; every other case returns Null.

## nominator_withdraw_absorb_payouts

`void nominator_withdraw_absorb_payouts(Block * produced, const ShaperMatch & m)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostNominator.cpp:60`
- callers: none found in parsed TU set
- calls: `mch::Block::compact_connections`, `mch::Block::merge_blocks`, `mch::ShaperMatch::capture`, `mch::classified_withdraw_payouts`

Shaper absorbing exactly the classified payout set into the produced withdraw block.

The wrapper merge has made the payouts siblings under the produced block, so the shaper temporarily chains them into the connected form merge_blocks requires and restores the original child and parent links once the combined frontier is derived. With no classified payouts it does nothing.
