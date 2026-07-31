// Cocoon host fns (builders/cocoon.py). See host/HostImpls.h for the internal
// registry surface and HostRegistry.h for the public one.
//
// Two fns, both for things the expression language cannot do by design:
// string formatting (§9.6) and a nested cell parse (§9.1).
#include "host/HostImpls.h"

#include "host/HostCommon.h"

#include "BlockTree.h"
#include "BuildRuntime.h"
#include "parse/PSlice.h"

#include "vm/cellslice.h"

#include <string>
#include <vector>

namespace mch {

namespace {

constexpr std::uint32_t kClientProxyRequestOp = 0x65448FF4;
constexpr std::uint32_t kClientProxyRefundGrantedOp = 0xC68EBC7B;

// "extern:<len>:<hex>" (the MsgParse.h contract for an addr_extern) ->
// Python's `f"{addr.len};{hex(addr.external_address)[2:]}"`. Both halves are
// already in the Value; only the separator differs (colon -> SEMICOLON).
Value extern_address_string(const std::string &s) {
  static const std::string kPrefix = "extern:";
  if (s.compare(0, kPrefix.size(), kPrefix) != 0) {
    return Value::null();
  }
  auto sep = s.find(':', kPrefix.size());
  if (sep == std::string::npos) {
    return Value::null();
  }
  std::string out = s.substr(kPrefix.size());
  out[sep - kPrefix.size()] = ';';
  return Value::make_str(std::move(out));
}

}  // namespace

// `expected_address` for cocoon_worker_payout / cocoon_proxy_charge /
// cocoon_grant_refund. The three legacy builds render `expectedMyAddress`
// identically (blocks/cocoon.py:134, :373, :1170):
//   ExternalAddress -> f"{len};{hex(value)[2:]}"
//   Address         -> to_str(False).upper()   == the canonical "wc:HEX"
//   otherwise (addr_none, i.e. a pytoniq None) -> left as None
EvalResult cocoon_expected_address(BuildEnv &, const std::vector<Value> &args) {
  if (args.size() != 1) {
    return rt_fault("cocoon_expected_address: bad arguments");
  }
  const Value &v = args[0];
  if (v.t == VType::Account) {
    return rt_ok(v.addr_none ? Value::null() : Value::make_str(v.str));
  }
  if (v.t == VType::Str) {
    return rt_ok(extern_address_string(v.str));
  }
  return rt_ok(Value::null());
}

// `withdraw_amount` for cocoon_client_withdraw: the coins carried inside
// ClientProxyRequest.payload, which is a `maybe_ref` that must then be parsed as
// ClientProxyRefundGranted. Reproduces the whole try/except of
// CocoonClientWithdrawMatcher.build_block (blocks/cocoon.py:1066-1077),
// including BOTH opcode asserts the two Python classes carry — every failure
// path returns null, and the spec's `?? 0` supplies legacy's fallback.
EvalResult cocoon_withdraw_amount(BuildEnv &, const std::vector<Value> &args) {
  if (args.size() != 1) {
    return rt_fault("cocoon_withdraw_amount: bad arguments");
  }
  const Block *req = as_block(args[0]);
  if (req == nullptr) {
    return rt_ok(Value::null());
  }
  auto r_body = block_body(req);
  if (r_body.is_error()) {
    return rt_ok(Value::null());
  }
  auto r_ctx = open_body(r_body.move_as_ok());
  if (r_ctx.is_error()) {
    return rt_ok(Value::null());
  }
  vm::CellSlice cs = std::move(r_ctx.move_as_ok().cs);
  // ClientProxyRequest: op, queryId, ownerAddress, stateData (^Cell), payload (Maybe ^Cell).
  if (!cs.have(32) || cs.fetch_ulong(32) != kClientProxyRequestOp) {
    return rt_ok(Value::null());
  }
  if (!cs.have(64) || !cs.advance(64)) {
    return rt_ok(Value::null());
  }
  if (load_address_py(cs).is_error()) {
    return rt_ok(Value::null());
  }
  if (cs.size_refs() == 0) {
    return rt_ok(Value::null());  // stateData is a MANDATORY ref (load_ref)
  }
  cs.fetch_ref();
  if (!cs.have(1) || cs.fetch_ulong(1) == 0 || cs.size_refs() == 0) {
    return rt_ok(Value::null());  // no payload -> legacy's `if client_req.payload:`
  }
  auto r_payload = open_ref_cell(cs.fetch_ref());
  if (r_payload.is_error()) {
    return rt_ok(Value::null());
  }
  // ClientProxyRefundGranted: op, coins, sendExcessesTo. The trailing address is
  // read (and can fail the whole parse) exactly as the Python class does.
  vm::CellSlice ps = r_payload.move_as_ok();
  if (!ps.have(32) || ps.fetch_ulong(32) != kClientProxyRefundGrantedOp) {
    return rt_ok(Value::null());
  }
  auto r_coins = load_coins_py(ps);
  if (r_coins.is_error()) {
    return rt_ok(Value::null());
  }
  if (load_address_py(ps).is_error()) {
    return rt_ok(Value::null());
  }
  return rt_ok(Value::make_int(r_coins.move_as_ok()));
}

}  // namespace mch
