---
module: TraceAggregates
source: ton-index-worker/ton-mch-engine/src/TraceAggregates.cpp
source-commit: 06d4c50
---

# TraceAggregates

TraceAggregates derives the trace-level aggregate fields that every loader contract must populate: start and end logical time, start and end utime, and the final masterchain sequence number. Keeping the fold in one place lets the schema loader and the fixture loader share it instead of each recomputing the values, and the module is deliberately free of lz4 and msgpack dependencies so the schema path can call it without pulling in the binary trace format. It is a pure in-place computation over the Trace's transaction list with no error contract to speak of; callers run it once after a transaction list is assembled, single-threaded like the rest of classification.

## fill_trace_aggregates

`void fill_trace_aggregates(Trace & trace)`

- decl: `ton-index-worker/ton-mch-engine/src/TraceAggregates.cpp:9`
- callers: mch::test_unknown_action (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:869); [mch::schema_to_trace (ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp:152)](SchemaTraceLoader.md#schema_to_trace)

Folds the transaction list into the five trace-level aggregate fields: min and max lt, min and max utime, and maximum masterchain seqno.

One pass over trace.transactions sets start_lt and end_lt to the min and max transaction lt, start_utime and end_utime to the min and max tx now, and mc_seqno_end to the maximum mc_block_seqno; a first-transaction flag bootstraps each field from the first tx rather than from zero. An empty transaction list leaves every field at its default. Never throws and touches nothing but the five aggregate fields.
