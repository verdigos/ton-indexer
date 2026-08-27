---
module: EmuTraceConvert
source: ton-index-worker/ton-mch-engine/src/emu/EmuTraceConvert.cpp
source-commit: 06d4c50
---

# EmuTraceConvert

Single-function module converting an emulated trace view into the schema transactions the classifier consumes. For each node it validates the transaction BOC, deserializes it, and parses it with ParseQuery at version 12, which selects only message fields no downstream consumer reads, then stamps the masterchain seqno from the node. All-or-nothing contract: the first bad node fails the whole conversion with an index-prefixed error. Pure function, no state.

## emu_to_schema_txs

`td::Result<std::vector<schema::Transaction>> emu_to_schema_txs(const EmuTraceView & view)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuTraceConvert.cpp:8`
- callers: [mch::EmuClassifierActor::classify (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:172)](EmuClassifierActor.md#classify)
- calls: `ParseQuery::parse_transaction`

Converts every view node into a schema transaction, failing the whole trace on the first bad node.

Reserves the node count and walks the nodes in order: a missing or empty tx_boc is an error, the BOC must deserialize, and parse errors are prefixed with the node index. The parsed transaction's mc_seqno is overwritten from the node because the BOC does not carry it. Returns the full vector only when every node parsed; any error aborts with no partial output.
