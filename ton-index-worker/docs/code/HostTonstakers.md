---
module: HostTonstakers
source: ton-index-worker/ton-mch-engine/src/host/HostTonstakers.cpp
source-commit: 06d4c50
---

# HostTonstakers

Host binding for Tonstakers liquid-staking withdrawals that are driven by burning the withdrawal NFT. The single host fn tonstakers_pool_addr recovers the staking pool account by walking the block chain upward from the NFT-burn notification: the start-asset-distribution call contributes its message source, an already-built tonstakers_withdraw block short-circuits with its pool field, and the walk continues only through the burn-notification, burn, and distributed-asset call opcodes. Any other block, or reaching the top of the chain, ends the walk with Null; the fn never faults.

## tonstakers_pool_addr

`EvalResult tonstakers_pool_addr(BuildEnv &, const std::vector<Value> & args)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostTonstakers.cpp:19`
- callers: none found in parsed TU set
- calls: `mch::Value::null`, `mch::account_from_opt`, `mch::as_block`, `mch::block_msg`, `mch::data_field`, `mch::is_call_op`, `mch::rt_ok`

Recovers the staking pool account by walking up from the NFT-burn notification to the start-asset-distribution call.

The start call, opcode 0x1140A64F, contributes its message source account, and an already-built tonstakers_withdraw block short-circuits with its pool field. The walk continues only through the burn-notification, burn, and distributed-asset opcodes 0xED58B0B2, 0xF127FE4E, and 0xDB3B8ABD; any other block, or reaching the top, returns Null.
