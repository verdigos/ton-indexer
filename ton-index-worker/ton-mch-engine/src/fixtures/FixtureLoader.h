// TEST-ONLY. Part of mch-fixtures; never linked into mch-classify or any
// product binary. See ton-mch-engine/CMakeLists.txt MCH_FIXTURE_SOURCES.
//
// Reads an .lz4 trace fixture (LZ4-frame-compressed msgpack) produced by the
// Python indexer's recorder (indexer/tests/traces/*.lz4, schema per
// tests/utils/trace_deserializer.py) into the engine's Trace data model
// (TraceLoader.h, which stays product-side). Mirrors mch-go-poc/trace/load.go:
// only the fields needed for structural matching are decoded; unknown msgpack
// keys are ignored — plus the root `interfaces` section, the per-account
// interface data Python's TestInterfaceRepository serves lookups from.
//
// The PRODUCTION loader is SchemaTraceLoader (schema::Transaction -> Trace);
// nothing here participates in it.
#pragma once

#include "BlockTree.h"  // TraceContext
#include "TraceLoader.h"

#include "td/utils/Status.h"

#include <string>

namespace mch {

// Reads, decompresses and decodes one fixture file.
td::Result<Trace> load_trace(const std::string &path);

// Fixture path -> the full block substrate (Trace -> EventTree -> BlockArena).
// tree.root == nullptr (empty trace) is an error. Lifted out of BlockTree.cpp:
// a product TU must not reference the fixture reader, or every target that
// links mch-classify drags lz4/msgpack in with it.
td::Result<TraceContext> load_trace_context(const std::string &path);

}  // namespace mch
