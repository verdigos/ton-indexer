---
module: HostLayerZero
source: ton-index-worker/ton-mch-engine/src/host/HostLayerZero.cpp
source-commit: 06d4c50
---

# HostLayerZero

Binds the LayerZero bridge address check to the engine with the single arity-2 host fn layerzero_dst_oapp_matches, registered in the host_fns map and dispatched by rt_call_hostfn after its arity check. It compares two destination oAPP address renderings for exact equality after stripping each side's two-character prefix and lowercasing; no zero-padding normalization happens, so a 64-digit padded hash deliberately fails to match the minimal form of the same hash, and a null on either side compares false.

## layerzero_dst_oapp_matches

`EvalResult layerzero_dst_oapp_matches(BuildEnv &, const std::vector<Value> & args)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostLayerZero.cpp:13`
- callers: none found in parsed TU set
- calls: `mch::(lambda at /home/verdigos/dev/ton-indexer-private/ton-index-worker/ton-mch-engine/src/host/HostLayerZero.cpp:17:22)::operator()`, `mch::Value::is_null`, `mch::Value::make_bool`, `mch::rt_ok`

Compares two destination oAPP address values for equality after prefix stripping and lowercasing.

A null on either side returns false outright. Each side renders through py_str_tail: an Account contributes its canonical upper-case string or the addr_none sentinel, any other value its string field; the first two characters are dropped and the remainder lowercased, which removes both an account workchain prefix and a 0x prefix. The comparison is literal string equality, deliberately unequal when a leading zero byte is padded on one side only.

## operator()

`std::string operator()(const Value & v) const`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostLayerZero.cpp:17`
- callers: mch::layerzero_dst_oapp_matches (ton-index-worker/ton-mch-engine/src/host/HostLayerZero.cpp:30)

Renders one address value as the lowercase hex tail used by the comparison.

