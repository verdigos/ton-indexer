// JVault host fns (builders/jvault.py). See host/HostImpls.h for the internal
// registry surface and HostRegistry.h for the public one.
//
// Both fns became portable in the F1+F2 rewrite: they take the composed
// jetton_transfer's `data` scalars (a b64 string / an already-parsed address
// list) rather than reaching the legacy `.jetton_transfer_message` attribute the
// generic IR block lacks — see docs/mch-qol-report.md.
#include "host/HostImpls.h"

#include "Value.h"

#include "td/utils/base64.h"
#include "vm/boc.h"
#include "vm/cellslice.h"

#include <string>
#include <vector>

namespace mch {

// builders/jvault.py jvault_stake_period(forward_payload_b64): decode the b64
// BOC of the transfer forward_payload, skip the 32-bit op, read the 32-bit
// stake period. None -> null (an absent forward_payload). A malformed payload
// raises in Python -> EvalError -> match rejection; here rt_fault does the same.
EvalResult jvault_stake_period(BuildEnv &, const std::vector<Value> &args) {
  if (args.size() != 1) {
    return rt_fault("jvault_stake_period: bad arguments");
  }
  if (args[0].is_null()) {
    return rt_ok(Value::null());
  }
  if (args[0].t != VType::Str) {
    return rt_fault("jvault_stake_period: expected a b64 string");
  }
  auto raw = td::base64_decode(td::Slice(args[0].str));
  if (raw.is_error()) {
    return rt_fault("jvault_stake_period: base64 decode failed");
  }
  auto cell = vm::std_boc_deserialize(raw.move_as_ok());
  if (cell.is_error()) {
    return rt_fault("jvault_stake_period: BOC deserialize failed");
  }
  vm::CellSlice cs = vm::load_cell_slice(cell.move_as_ok());
  if (!cs.have(32 + 32) || !cs.advance(32)) {
    return rt_fault("jvault_stake_period: forward_payload underflow");
  }
  return rt_ok(Value::make_int64(static_cast<std::int64_t>(cs.fetch_ulong(32))));
}

// builders/jvault.py jvault_account_list(addresses): [AccountId(a) for a in xs].
// The parsed JVaultClaim body already carries jettons_to_claim as account
// Values (load_address_py), so the AccountId wrap is an identity over the list
// here (the render already matches AccountId.as_str()); the fn exists only
// because the element-wise constructor-map `map(xs as e => account(e))` is
// deferred G-LC (spec §16.1). Non-list / null -> fault (Python iterating those
// raises).
EvalResult jvault_account_list(BuildEnv &, const std::vector<Value> &args) {
  if (args.size() != 1) {
    return rt_fault("jvault_account_list: bad arguments");
  }
  if (args[0].t != VType::List || args[0].items == nullptr) {
    return rt_fault("jvault_account_list: expected a list");
  }
  std::vector<Value> out;
  out.reserve(args[0].items->size());
  for (const auto &e : *args[0].items) {
    if (e.t == VType::Account) {
      out.push_back(e);
    } else if (e.t == VType::Str) {
      out.push_back(Value::make_account_raw(e.str));
    } else if (e.is_null()) {
      out.push_back(Value::make_account_none());
    } else {
      return rt_fault("jvault_account_list: element is not an address");
    }
  }
  return rt_ok(Value::make_list(std::move(out)));
}

}  // namespace mch
