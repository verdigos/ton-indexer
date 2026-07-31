// EVAA host predicates (builders/evaa.py). See host/HostImpls.h for the
// internal registry surface and HostRegistry.h for the public one.
#include "host/HostImpls.h"

#include "host/HostCommon.h"

#include "BlockTree.h"
#include "BuildRuntime.h"
#include "MsgParse.h"
#include "TraceLoader.h"

#include "common/refint.h"
#include "vm/cellslice.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mch {

namespace {

// load_user_header: skip the EVAA user header (version coins + maybe
// upgrade-info ref + 2 exec bits), leaving the cursor on the 32-bit opcode.
bool skip_user_header(vm::CellSlice &cs) {
  if (pyslice_load_coins(cs).is_error()) {  // user_version
    return false;
  }
  if (!cs.have(1)) {  // load_maybe_ref: the Maybe bit
    return false;
  }
  if (cs.fetch_ulong(1)) {
    if (cs.size_refs() == 0) {
      return false;
    }
    cs.fetch_ref();  // upgrade_info
  }
  if (!cs.have(2)) {  // upgrade_exec
    return false;
  }
  cs.advance(2);
  return true;
}

// Open a user-contract body positioned on its 32-bit opcode.
bool open_user_body(const Block *b, vm::CellSlice &cs) {
  if (b->btype != "call_contract") {  // isinstance(block, CallContractBlock)
    return false;
  }
  auto r_body = block_body(b);
  if (r_body.is_error()) {
    return false;
  }
  bool special = false;
  try {
    cs = vm::load_cell_slice_special(r_body.ok(), special);
  } catch (...) {
    return false;
  }
  return skip_user_header(cs);
}

// _opcode_after_user_header: skip the header, then compare the 32-bit opcode.
// Any malformed body never matches (Python's blanket except).
bool opcode_after_user_header(const Block *b, td::uint32 opcode) {
  vm::CellSlice cs;
  if (!open_user_body(b, cs)) {
    return false;
  }
  if (!cs.have(32)) {
    return false;
  }
  return static_cast<td::uint32>(cs.fetch_ulong(32)) == opcode;
}

constexpr td::uint32 kSupplyMaster = 0x1;
constexpr td::uint32 kSupplyUser = 0x11;
constexpr td::uint32 kSupplySuccess = 0x11a;
constexpr td::uint32 kSupplyFail = 0x11f;

}  // namespace

bool evaa_user_withdraw_user(const Block *b) {
  return opcode_after_user_header(b, 0x21);  // EvaaWithdrawUser.opcode
}

bool evaa_user_withdraw_success(const Block *b) {
  return opcode_after_user_header(b, 0x211a);  // EvaaWithdrawSuccess.opcode
}

bool evaa_user_withdraw_fail(const Block *b) {
  return opcode_after_user_header(b, 0x211f);  // EvaaWithdrawFail.opcode
}

// evaa_service_comment: a TonTransferBlock whose comment is one of the EVAA
// service comments (blocks/evaa.py EVAA_ACTION_COMMENTS). Comment derivation
// mirrors TonTransferBlock exactly (ton_transfer_comment).
bool evaa_service_comment(const Block *b) {
  if (b->btype != "ton_transfer") {
    return false;
  }
  auto r_body = block_body(b);
  if (r_body.is_error()) {
    return false;
  }
  auto comment = ton_transfer_comment(r_body.ok());
  if (!comment) {
    return false;
  }
  return *comment == "EVAA liquidation." || *comment == "EVAA supply." ||
         *comment == "EVAA withdraw.";
}

bool evaa_user_supply(const Block *b) {
  return opcode_after_user_header(b, kSupplyUser);
}

// EvaaSupplyBlockMatcher.test_self: the 0x1 supply_master call itself (TON side),
// or a jetton transfer whose FORWARD PAYLOAD starts with the supply_master
// opcode (jetton side). btype, not isinstance — the legacy JettonTransferBlock
// check is dead under the IR engine.
bool evaa_supply_anchor(const Block *b) {
  if (is_call_op(b, kSupplyMaster)) {
    return true;
  }
  if (b->btype != "jetton_transfer") {
    return false;
  }
  Value fp = data_field(b, "forward_payload");
  if (fp.t != VType::Str) {
    return false;
  }
  auto r_cell = cell_from_pystr(fp.str);
  if (r_cell.is_error()) {
    return false;
  }
  bool special = false;
  vm::CellSlice cs;
  try {
    cs = vm::load_cell_slice_special(r_cell.ok(), special);
  } catch (...) {
    return false;
  }
  return cs.have(32) && static_cast<td::uint32>(cs.fetch_ulong(32)) == kSupplyMaster;
}

// builders/evaa.py _evaa_supply_data -> blocks/evaa.py build_evaa_supply_core.
// The whole build of specs/evaa.mch's `evaa_supply`: the TON-vs-jetton
// discriminator threads through nearly every field, and the jetton arm has to
// parse a forward-payload BOC, which the DSL has no form for. Python wraps the
// body in try/except returning None, so any parse/nav failure is a Null reject.
EvalResult evaa_supply_data(BuildEnv &env, const std::vector<Value> &args) {
  (void)env;
  if (args.size() != 1 || args[0].t != VType::List || args[0].items->empty()) {
    return host_reject("bad arguments");
  }
  std::vector<const Block *> consumed;
  for (const Value &v : *args[0].items) {
    const Block *b = as_block(v);
    if (b != nullptr) {
      consumed.push_back(b);
    }
  }
  if (consumed.empty()) {
    return host_reject("empty consumed");
  }
  const Block *block = consumed.front();

  // Label roles, derived structurally from the consumed set (the anchor is
  // excluded — on the jetton arm it is itself a jetton_transfer).
  const Block *user = nullptr, *success = nullptr, *fail = nullptr, *refund = nullptr;
  for (std::size_t i = 1; i < consumed.size(); i++) {
    const Block *b = consumed[i];
    if (user == nullptr && opcode_after_user_header(b, kSupplyUser)) user = b;
    if (success == nullptr && is_call_op(b, kSupplySuccess)) success = b;
    if (fail == nullptr && is_call_op(b, kSupplyFail)) fail = b;
    if (refund == nullptr && b->btype == "jetton_transfer") refund = b;
  }
  if (user == nullptr) {
    return host_reject("no supply_user block");
  }

  bool is_ton = block->btype == "call_contract";
  Value sender, recipient, master, asset;
  Value sender_jetton_wallet = Value::null();
  Value recipient_jetton_wallet = Value::null();
  Value master_jetton_wallet = Value::null();
  td::RefInt256 amount;

  const Message *bm = block_msg(block);
  if (is_ton) {
    auto rb = block_body(block);
    if (rb.is_error()) {
      return host_reject("supply_master body");
    }
    auto rm = parse_message_body("EvaaSupplyMaster", rb.ok());
    if (rm.is_error()) {
      return host_reject("supply_master parse");
    }
    Value m = rm.move_as_ok();
    const Value *sa = m.field("supply_amount");
    const Value *ra = m.field("recipient_address");
    if (sa == nullptr || ra == nullptr) {
      return host_reject("supply_master fields");
    }
    sender = account_from_opt(bm != nullptr ? bm->source : std::nullopt);
    amount = sa->num;
    recipient = *ra;
    master = account_from_opt(bm != nullptr ? bm->destination : std::nullopt);
    asset = Value::make_asset_ton();
  } else {
    sender = data_field(block, "sender");
    sender_jetton_wallet = data_field(block, "sender_wallet");
    master_jetton_wallet = data_field(block, "receiver_wallet");
    Value amt = data_field(block, "amount");
    amount = amt.num;  // Amount.value
    master = data_field(block, "receiver");
    asset = data_field(block, "asset");
    Value fp = data_field(block, "forward_payload");
    if (fp.t != VType::Str) {
      return host_reject("no forward_payload in jetton supply");
    }
    auto r_cell = cell_from_pystr(fp.str);
    if (r_cell.is_error()) {
      return host_reject("forward_payload BOC");
    }
    auto rm = parse_message_body("EvaaSupplyJettonForward", r_cell.ok());
    if (rm.is_error()) {
      return host_reject("forward_payload parse");
    }
    const Value *ra = rm.ok().field("recipient_address");
    if (ra == nullptr) {
      return host_reject("forward_payload recipient");
    }
    recipient = *ra;
    if (same_account(sender, recipient)) {
      recipient_jetton_wallet = sender_jetton_wallet;
    } else {
      // Supplying on someone else's behalf: name their wallet from the sibling
      // transfer the master sent out. (No corpus coverage; ported for parity.)
      for (std::size_t i = 1; i < consumed.size(); i++) {
        const Block *b = consumed[i];
        if (b->btype != "jetton_transfer") continue;
        if (same_account(data_field(b, "sender_wallet"), master_jetton_wallet)) {
          recipient_jetton_wallet = data_field(b, "receiver_wallet");
          break;
        }
      }
    }
  }

  const Message *um = block_msg(user);
  Value recipient_contract =
      account_from_opt(um != nullptr ? um->destination : std::nullopt);

  // The user's asset_id sits behind the variable header, so no schema entry.
  td::RefInt256 asset_id;
  {
    vm::CellSlice cs;
    if (!open_user_body(user, cs)) {
      return host_reject("supply_user body");
    }
    if (!cs.have(32 + 64 + 256)) {
      return host_reject("supply_user underflow");
    }
    cs.advance(32 + 64);  // opcode + query_id
    asset_id = cs.fetch_int256(256, false);
    if (asset_id.is_null()) {
      return host_reject("supply_user asset_id");
    }
  }

  if (success != nullptr) {
    auto rb = block_body(success);
    if (rb.is_error()) {
      return host_reject("supply_success body");
    }
    auto rs = parse_message_body("EvaaSupplySuccess", rb.ok());
    if (rs.is_error()) {
      return host_reject("supply_success parse");
    }
    const Value *as = rs.ok().field("amount_supplied");
    if (as == nullptr) {
      return host_reject("supply_success amount");
    }
    amount = as->num;  // the authoritative amount
  } else if (fail == nullptr && refund == nullptr) {
    return host_reject("supply not completed");  // no outcome yet
  }

  Value::Fields d;
  d.emplace_back("sender", std::move(sender));
  d.emplace_back("recipient", std::move(recipient));
  d.emplace_back("recipient_contract", std::move(recipient_contract));
  d.emplace_back("amount", Value::make_int(std::move(amount)));
  d.emplace_back("is_success", Value::make_bool(success != nullptr));
  d.emplace_back("is_ton", Value::make_bool(is_ton));
  d.emplace_back("asset_id", Value::make_int(std::move(asset_id)));
  d.emplace_back("sender_jetton_wallet", std::move(sender_jetton_wallet));
  d.emplace_back("recipient_jetton_wallet", std::move(recipient_jetton_wallet));
  d.emplace_back("master_jetton_wallet", std::move(master_jetton_wallet));
  d.emplace_back("master", std::move(master));
  d.emplace_back("asset", std::move(asset));
  return rt_ok(Value::make_obj(std::move(d)));
}

}  // namespace mch
