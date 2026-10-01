#pragma once
#include <cstddef>
#include <exception>
#include <utility>

#include "td/utils/logging.h"
#include "td/utils/Status.h"
#include "vm/dict.h"
#include "vm/excno.hpp"

// Helpers for parsing cells that come from contract get-methods or account data.
// Such cells are controlled by whoever deployed the contract, so parsing must neither
// throw out of a detector nor do unbounded work.
namespace safe_parse {

// A dictionary of n distinct cells can encode up to 2^n entries by pointing both
// branches of a fork at the same subtree, so entries are capped independently of size.
constexpr std::size_t kDefaultMaxDictEntries = 1024;

// Runs f() and turns any exception into an error prefixed with `what`.
// Catching vm::VmError alone is not enough: vm::VmVirtError (pruned branch),
// vm::CellBuilder::CellWriteError and vm::VmFatal do not derive from it.
template <class F>
auto guard(td::Slice what, F&& f) -> decltype(f()) {
  try {
    return f();
  } catch (const vm::VmError& e) {
    return td::Status::Error(PSLICE() << what << ": " << e.get_msg());
  } catch (const vm::VmVirtError& e) {
    return td::Status::Error(PSLICE() << what << ": " << e.get_msg());
  } catch (const std::exception& e) {
    return td::Status::Error(PSLICE() << what << ": " << e.what());
  } catch (...) {
    return td::Status::Error(PSLICE() << what << ": unknown exception");
  }
}

// Calls on_entry(key, key_len, value) for each dictionary entry in ascending key order.
// key is valid only during the call. Fails after max_entries entries, on a malformed
// dictionary, or on the first error returned by on_entry. Never throws.
template <class F>
td::Status for_each_dict_entry(td::Ref<vm::Cell> root, int key_bits, std::size_t max_entries, F&& on_entry) {
  return guard("dictionary", [&]() -> td::Status {
    vm::Dictionary dict{std::move(root), key_bits};
    td::Status status;
    std::size_t seen = 0;
    bool complete = dict.check_for_each([&](td::Ref<vm::CellSlice> value, td::ConstBitPtr key, int key_len) {
      if (++seen > max_entries) {
        status = td::Status::Error(PSLICE() << "more than " << max_entries << " entries");
        return false;
      }
      status = on_entry(key, key_len, std::move(value));
      return status.is_ok();
    });
    if (status.is_error()) {
      return status;
    }
    if (!complete) {
      return td::Status::Error("malformed dictionary");
    }
    return td::Status::OK();
  });
}

}  // namespace safe_parse
