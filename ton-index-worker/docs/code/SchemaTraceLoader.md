---
module: SchemaTraceLoader
source: ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp
source: ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.h
source-commit: 06d4c50
---

# SchemaTraceLoader

SchemaTraceLoader maps tondb-scanner's schema transactions and messages into the engine's own Trace structures so that to_tree, init_block and ClassifyCore run unchanged on production data. The input is the trace's already-gathered schema transaction list rather than schema::Trace, because the engine rebuilds the event-node tree purely from message-hash linking and has no use for the edge list. schema_to_trace is a pure value transformation that never throws: message bodies are reused from the already-serialized BOC strings with no cell re-serialization, and trace-level lt/utime aggregates are derived at the end through fill_trace_aggregates. Interfaces are deliberately left empty here — filling them is the lookup source's concern.

## class mch::DescrInfo

- decl: `ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:29`

Intermediate holding the flattened fields extracted from a schema transaction description variant.

descr is one of ord, tick_tock or other; exit_code, skipped_reason and aborted are populated only by the ord and tick_tock arms and stay unset otherwise. Instances are produced by descr_info and consumed field-by-field when filling a Transaction; it is a plain value type whose only ownership is its strings.

## b256

`std::string b256(const td::Bits256 & h)`

- decl: `ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:13`
- callers: mch::map_message (ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:103); mch::map_message (ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:108); mch::schema_to_trace (ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:122); mch::map_message (ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:81)

Base64-encodes a 256-bit hash into the engine's string hash fields.

One-line forwarder onto base64_encode over the hash slice, used for transaction and message hashes and for the init-state cell hash. Deterministic and infallible; the only cost is the allocation for the encoded string.

## status_str

`std::string status_str(schema::AccountStatus s)`

- decl: `ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:15`
- callers: mch::schema_to_trace (ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:128); mch::schema_to_trace (ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:129)

Maps an account-status enum value to its lowercase engine string.

Returns uninit, frozen, active or nonexist for the four known enum values and the empty string for anything else, which the engine reads as an unknown status. Pure lookup with no failure mode.

## DescrInfo

`void DescrInfo()`

- decl: `ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:29`
- callers: mch::descr_info (ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:39)

Implicitly default-constructed; descr is empty and the optionals and aborted are unset.


## DescrInfo

`void DescrInfo(DescrInfo &&)`

- decl: `ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:29`
- callers: mch::descr_info (ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:76)

Implicit move constructor; moves the strings and optionals.


## descr_info

`DescrInfo descr_info(const schema::TransactionDescr & d)`

- decl: `ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:38`
- callers: mch::schema_to_trace (ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:130)
- calls: `mch::(lambda at /home/verdigos/dev/ton-indexer-private/ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:40:18)::operator()`, `mch::(lambda at /home/verdigos/dev/ton-indexer-private/ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:46:28)::operator()`, `mch::DescrInfo::DescrInfo`

Flattens a schema transaction description variant into the descr tag, exit code, skip reason and aborted flag.

The ord and tick_tock arms pull the shared compute phase through two local lambdas, while every other description variant collapses to descr other with nothing else filled. abort flags come straight from the matching arm. Pure value computation over the variant; it cannot fail or throw.

## operator()

`std::optional<std::int64_t> operator()(const schema::TrComputePhase & cp) const`

- decl: `ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:40`
- callers: mch::descr_info (ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:65); mch::descr_info (ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:70)

Returns the vm compute phase's exit code, nullopt when the phase was skipped.

Uses get_if for the vm alternative only, so a skipped phase reports no exit code. Evaluated at most once per description arm inside descr_info.

## operator()

`std::optional<std::string> operator()(const schema::TrComputePhase & cp) const`

- decl: `ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:46`
- callers: mch::descr_info (ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:66); mch::descr_info (ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:71)

Names a skipped compute phase's reason, nullopt when the phase actually ran.

Matches the skip-reason enum to no_state, bad_state, no_gas or suspended. Any other reason value leaves the optional unset even for a skipped phase; the fallback return is unreachable for well-formed input.

## map_message

`void map_message(Message * m, const schema::Message & s, const std::string & owner_tx_hash, std::int64_t owner_tx_lt, const char * direction)`

- decl: `ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:79`
- callers: mch::schema_to_trace (ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:139); mch::schema_to_trace (ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:145)
- calls: `mch::MsgContent::MsgContent`, `mch::b256`

Copies one schema message onto an engine Message, tagging it with its owning transaction and direction.

Fills the message hash, owner tx hash and lt, in/out direction, endpoints, widened opcode, grams-only value with extra currencies reduced to a flag, timestamps, and bounce flags. The body is taken from the already-serialized BOC string — no cell re-serialization — and an init state is attached only when both the BOC string and the parsed state cell are present, keyed by the state cell's own hash. Void and infallible; writes through the caller-owned Message pointer.

## schema_to_trace

`td::Result<Trace> schema_to_trace(const std::string & trace_id, const std::vector<schema::Transaction> & txs)`

- decl: `ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.h:27`
- callers: mch::test_schema_no_gas_ton_transfer (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:943); mch::schema_ctx_loader (ton-index-worker/ton-mch-engine/harness/SchemaGate.cpp:154); [mch::classify_schema_trace (ton-index-worker/ton-mch-engine/src/ClassifyCore.cpp:826)](ClassifyCore.md#classify_schema_trace)
- calls: `convert::to_raw_address`, `mch::Trace::Trace`, `mch::b256`, `mch::canonicalize_or_passthrough`, `mch::descr_info`, `mch::fill_trace_aggregates`, `mch::map_message`, `mch::status_str`

Converts a trace's schema transactions into an engine Trace ready for tree building and classification.

Builds one Transaction per schema entry — hash, lt, now, mc seqno, canonical-or-original raw account, both status strings and the flattened descr_info fields — then attaches the inbound message and every outbound message through map_message, each holding a raw back-pointer to its owning transaction. fill_trace_aggregates derives the trace-level lt/utime aggregates afterwards. Interfaces stay empty for the lookup source to fill; the Result wrapper has no error path in the current body, so every input converts.
