#include "AbiLeavesPrefix.h"

#include "vm/cells/CellSlice.h"

namespace ton_abi {

namespace {

// Read `len` bits (len <= 1023) as an unsigned value, requiring the high
// (len-64) bits to be zero when len > 64 (prefix_num fits in uint64). Returns
// the low <=64-bit value in `out`. Fail-closed on truncation.
td::Status fetch_prefix_bits(vm::CellSlice &cs, int len, std::uint64_t &out) {
  if (len < 0 || len > 1023) {
    return td::Status::Error(PSLICE() << "prefix: length " << len << " out of [0,1023]");
  }
  if (!cs.have(static_cast<unsigned>(len))) {
    return td::Status::Error(PSLICE() << "prefix: not enough bits for a " << len << "-bit prefix");
  }
  int high = len - 64;
  while (high > 0) {
    int chunk = high > 64 ? 64 : high;
    unsigned long long hz = 0;
    if (!cs.fetch_ulong_bool(static_cast<unsigned>(chunk), hz) || hz != 0) {
      return td::Status::Error("prefix: high bits of a >64-bit prefix must be zero");
    }
    high -= chunk;
  }
  int low = len > 64 ? 64 : len;
  out = 0;
  if (low > 0) {
    if (!cs.fetch_ulong_bool(static_cast<unsigned>(low), out)) {
      return td::Status::Error("prefix: failed to read prefix bits");
    }
  }
  return td::Status::OK();
}

}  // namespace

td::Status load_and_check_prefix(vm::CellSlice &cs, std::uint64_t num, int len) {
  std::uint64_t got = 0;
  TRY_STATUS(fetch_prefix_bits(cs, len, got));
  if (got != num) {
    return td::Status::Error(PSLICE() << "prefix: expected " << num << " (" << len << " bits), got " << got);
  }
  return td::Status::OK();
}

td::Status store_prefix(vm::CellBuilder &cb, std::uint64_t num, int len) {
  if (len < 0 || len > 1023) {
    return td::Status::Error(PSLICE() << "prefix: length " << len << " out of [0,1023]");
  }
  if (len == 0) {
    return td::Status::OK();
  }
  int high = len - 64;
  while (high > 0) {
    int chunk = high > 64 ? 64 : high;
    if (!cb.store_long_bool(0, static_cast<unsigned>(chunk))) {
      return td::Status::Error("prefix: cannot store high zero bits");
    }
    high -= chunk;
  }
  int low = len > 64 ? 64 : len;
  // store_long_bool stores the low `low` bits of the value's bit pattern; the
  // uint64 -> long long reinterpret is bit-preserving.
  if (!cb.store_long_bool(static_cast<long long>(num), static_cast<unsigned>(low))) {
    return td::Status::Error("prefix: cannot store prefix bits");
  }
  return td::Status::OK();
}

bool lookup_prefix(const vm::CellSlice &cs, std::uint64_t num, int len) {
  if (len < 0 || len > 64) {
    // Dispatch prefixes are loader-bounded to <= 32 bits; a peek wider than 64
    // is never a dispatch case. Treat as no-match rather than reading.
    return len == 0;  // empty prefix always "matches"
  }
  if (!cs.have(static_cast<unsigned>(len))) {
    return false;
  }
  if (len == 0) {
    return true;
  }
  return cs.prefetch_ulong(static_cast<unsigned>(len)) == num;
}

td::Result<UnionDispatch> dispatch_union(vm::CellSlice &cs, const ResolvedUnion &ru) {
  const bool empty = cs.size() == 0 && cs.size_refs() == 0;

  // Exact T | void (2 variants, last is void): empty -> void FIRST; else take
  // the T unconditionally, eating its implicit prefix.
  if (ru.has_void && ru.variants.size() == 2) {
    if (empty) {
      return UnionDispatch{ru.variants.size() - 1, true};
    }
    const LabeledVariant &t = ru.variants[0];
    if (t.is_prefix_implicit && t.prefix_len > 0) {
      if (!cs.advance(static_cast<unsigned>(t.prefix_len))) {
        return td::Status::Error("union: truncated eating T|void implicit prefix");
      }
    }
    return UnionDispatch{0, false};
  }

  // General: probe non-void variants in the sorted dispatch order.
  for (std::size_t idx : ru.dispatch_order) {
    const LabeledVariant &v = ru.variants[idx];
    if (!lookup_prefix(cs, v.prefix_num, v.prefix_len)) {
      continue;
    }
    if (v.is_prefix_implicit) {
      if (v.prefix_len > 0 && !cs.advance(static_cast<unsigned>(v.prefix_len))) {
        return td::Status::Error("union: truncated eating implicit prefix");
      }
    }
    // EXPLICIT: leave the bits for the variant's own loader to re-read.
    return UnionDispatch{idx, false};
  }

  // No prefix matched: void only if the slice is exhausted, else error.
  if (ru.has_void && empty) {
    return UnionDispatch{ru.variants.size() - 1, true};
  }
  return td::Status::Error("union: none of the variant prefixes match");
}

td::Status store_union_prefix(vm::CellBuilder &cb, const LabeledVariant &v) {
  if (v.is_prefix_implicit && v.prefix_len > 0) {
    return store_prefix(cb, v.prefix_num, v.prefix_len);
  }
  return td::Status::OK();  // explicit prefix written by the variant's own store
}

}  // namespace ton_abi
