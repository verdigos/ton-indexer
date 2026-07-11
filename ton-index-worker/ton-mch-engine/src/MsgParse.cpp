// TL-B message-parsing registry and dispatch (see MsgParse.h for the semantics
// contract). The per-family adapters live in src/parse/*; this file owns the
// message_parsers() map, parse_message_body dispatch, and the --parse-dump mode.
//
// FILE-LEVEL PYTONIQ-PARITY CATALOGUE (referenced by the split parser TUs):
// Every quirk of the Python reference parsers is reproduced deliberately:
//  - pytoniq Slice.copy() RESETS ref consumption, so JettonTransfer's INLINE
//    forward_payload sees ALL body refs from index 0 (including an already
//    consumed custom_payload ref) — both in to_cell() and in the snake walk;
//  - JettonTransfer.sum_type is a conditionally-present key (Python only sets
//    the attribute on some paths; notably NOT on stonfi-swap success and NOT
//    when the body has no payload bits);
//  - load_address: addr_none -> null, addr_extern -> "extern:<len>:<value>"
//    Str (len 0 fails: pytoniq ba2int on an empty bitarray raises), addr_var
//    fails, anycast is consumed but NOT applied (pytoniq stores it without
//    rewriting the address prefix, unlike convert::to_raw_address);
//  - load_coins with len nibble 0 yields 0 (not null);
//  - payload_sum_type is Python hex(): lowercase, no zero padding.
#include "MsgParse.h"

#include "TraceLoader.h"
#include "parse/PSlice.h"
#include "parse/Parsers.h"

#include "common/refint.h"
#include "td/utils/base64.h"
#include "vm/boc.h"
#include "vm/cellslice.h"
#include "vm/excno.hpp"

#include <exception>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mch {

const std::map<std::string, MsgParserFn> &message_parsers() {
  static const std::map<std::string, MsgParserFn> parsers = {
      {"JettonTransfer", parse_jetton_transfer},
      {"JettonInternalTransfer", parse_jetton_internal_transfer},
      {"JettonBurn", parse_jetton_burn},
      {"JettonNotify", parse_jetton_notify},
      {"JettonMint", parse_jetton_mint},
      {"MinterJettonMint", parse_minter_jetton_mint},
      {"JettonBurnNotification", parse_never},
      {"ChangeDnsRecordMessage", parse_change_dns},
      {"VestingSendMessage", parse_vesting_send_message},
      {"PTonTransfer", parse_pton_transfer},
      {"StonfiV2ProvideLiquidity", parse_stonfi_v2_provide_liquidity},
      {"DedustPayoutFromPool", parse_dedust_payout_from_pool},
      {"DedustSwapNotification", parse_dedust_swap_notification},
      {"CoffeeCreateVault", parse_coffee_create_vault},
      {"EvaaWithdrawMaster", parse_evaa_withdraw_master},
      {"EvaaWithdrawCollateralized", parse_evaa_withdraw_collateralized},
      {"EvaaWithdrawFailExcess", parse_evaa_withdraw_fail_excess},
      {"LayerZeroOappExecuteCallback", parse_layerzero_oapp_execute_callback},
      {"JVaultUnstakeJettons", parse_jvault_unstake_jettons},
      {"JVaultUnstakeRequest", parse_jvault_unstake_request},
      {"JVaultClaim", parse_jvault_claim},
  };
  return parsers;
}

td::Result<Value> parse_message_body(const std::string &type_name, const td::Ref<vm::Cell> &body) {
  auto it = message_parsers().find(type_name);
  if (it == message_parsers().end()) {
    return td::Status::Error(PSLICE() << "message type " << type_name << " is not registered");
  }
  // Soft-parse (§5.4): a malformed body must reduce to a clean parse failure,
  // but only for the exception kinds a parser legitimately raises — cell VM
  // errors and std::exception subclasses (out_of_range/logic_error from the
  // hand-written tails). Anything else (bad_alloc, unknown host faults) is an
  // infrastructure failure and propagates to the trace-level handler rather
  // than being misclassified as an ordinary parse mismatch.
  try {
    return it->second(body);
  } catch (const vm::VmError &e) {
    return e.as_status("parse threw: ");
  } catch (const vm::VmVirtError &) {
    return td::Status::Error("parse threw: vm virtualization error");
  } catch (const vm::VmNoGas &) {
    return td::Status::Error("parse threw: out of gas");
  } catch (const std::exception &e) {
    return td::Status::Error(PSLICE() << "parse threw: " << e.what());
  }
}

}  // namespace mch
