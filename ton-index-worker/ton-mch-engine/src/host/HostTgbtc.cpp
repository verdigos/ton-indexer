// tgbtc_mint_data derives head, success log, and jetton_mint from the consumed
// set. Event parsers reject unreadable bodies; TgBTCBurnEvent tolerates
// truncated addresses as addr_none.
#include "host/HostImpls.h"

#include "host/BlockViews.h"
#include "host/HostAdapter.h"
#include "host/HostCommon.h"

#include "BlockTree.h"
#include "BuildRuntime.h"
#include "HostRegistry.h"
#include "MsgParse.h"
#include "btypes_gen.h"
#include "parse/PSlice.h"
#include "parse/Parsers.h"

#include "common/bigint.hpp"
#include "common/refint.h"
#include "vm/cellslice.h"

#include <string>
#include <vector>

namespace mch {

namespace {

constexpr std::uint32_t kTgbtcMintHead = 0x3F781D24;
constexpr std::uint32_t kTgbtcMintSuccessLog = 0x77A80EF3;

// 32-byte little-endian, lowercase hex.
std::string txid_le_hex(const td::RefInt256 &v) {
  unsigned char le[32] = {0};
  v->export_bytes_lsb(le, 32, /*sgnd=*/false);
  static const char *hexd = "0123456789abcdef";
  std::string out(64, '0');
  for (int i = 0; i < 32; i++) {
    out[2 * i] = hexd[le[i] >> 4];
    out[2 * i + 1] = hexd[le[i] & 0xF];
  }
  return out;
}

// Account passes through; addr_none stays none; an external-address Str
// rejects (ok is cleared).
Value account_id_of(const Value &v, bool &ok) {
  if (v.t == VType::Account) {
    return v;
  }
  if (v.is_null()) {
    return Value::make_account_none();
  }
  ok = false;
  return Value::null();
}

// Guarded form: an extern address is addr_none here, never a reject.
Value account_id_or_none(const Value &v) {
  return v.t == VType::Account ? v : Value::make_account_none();
}

// TgBTCMintEvent tail after the 32-bit opcode: coins, recipient, uint256 txid.
// Any step failure leaves ok false.
struct MintLog {
  td::RefInt256 amount;
  Value recipient;
  td::RefInt256 txid;
  bool ok{false};
};

MintLog parse_mint_log(vm::CellSlice &cs) {
  MintLog out;
  auto r_amt = load_coins_py(cs);
  if (r_amt.is_error()) {
    return out;
  }
  auto r_rcpt = load_address_py(cs);
  if (r_rcpt.is_error()) {
    return out;
  }
  bool rcpt_ok = true;
  out.recipient = account_id_of(r_rcpt.ok(), rcpt_ok);
  if (!rcpt_ok || !cs.have(256)) {
    return out;
  }
  out.txid = cs.fetch_int256(256, false);
  if (out.txid.is_null()) {
    return out;
  }
  out.amount = r_amt.move_as_ok();
  out.ok = true;
  return out;
}

}  // namespace

// 6-field mint record, or Null if head/log/mint is missing or the log is
// unparseable.
EvalResult tgbtc_mint_data(BuildEnv &, const std::vector<Value> &args) {
  if (args[0].t != VType::List) {
    return rt_fault("tgbtc_mint_data: bad arguments");
  }
  auto decoded = decode_consumed_or_none(args);
  if (!decoded) {
    return rt_ok(Value::null());
  }
  std::vector<const Block *> consumed = std::move(decoded->blocks);
  const Block *head = first_call(consumed, kTgbtcMintHead);
  const Block *success_log = first_call(consumed, kTgbtcMintSuccessLog);
  const Block *jetton_mint = nullptr;
  for (const Block *b : consumed) {
    if (b->btype == mch::btype::kJettonMint) {
      jetton_mint = b;
      break;
    }
  }
  if (head == nullptr || success_log == nullptr || jetton_mint == nullptr) {
    return rt_ok(Value::null());  // core: missing piece -> reject
  }

  auto r_body = block_body(success_log);
  if (r_body.is_error()) {
    return rt_ok(Value::null());  // unreadable log -> Null reject
  }
  auto r_ctx = open_body(r_body.ok());
  if (r_ctx.is_error()) {
    return rt_ok(Value::null());
  }
  auto ctx = r_ctx.move_as_ok();
  auto &cs = ctx.cs;
  if (!cs.have(32)) {
    return rt_ok(Value::null());
  }
  cs.advance(32);
  auto parsed = parse_mint_log(cs);
  if (!parsed.ok) {
    return rt_ok(Value::null());
  }
  td::RefInt256 amount = std::move(parsed.amount);
  Value recipient = std::move(parsed.recipient);
  td::RefInt256 txid = std::move(parsed.txid);

  const Message *lmsg = block_msg(success_log);
  Value teleport_contract = account_from_opt(lmsg != nullptr ? lmsg->source : std::nullopt);
  std::string bitcoin_txid = txid_le_hex(txid);

  Value asset = data_field(jetton_mint, "asset");
  Value to_wallet = data_field(jetton_mint, "to_jetton_wallet");
  Value recipient_wallet =
      to_wallet.t == VType::Str ? account_from_opt(std::optional<std::string>(to_wallet.str))
      : to_wallet.t == VType::Account ? to_wallet
                                      : Value::make_account_none();

  Value::Fields ns;
  ns.emplace_back("recipient", std::move(recipient));
  ns.emplace_back("amount", Value::make_int(std::move(amount)));
  ns.emplace_back("asset", std::move(asset));
  ns.emplace_back("bitcoin_txid", Value::make_str(std::move(bitcoin_txid)));
  ns.emplace_back("recipient_wallet", std::move(recipient_wallet));
  ns.emplace_back("teleport_contract", std::move(teleport_contract));
  return rt_ok(Value::make_obj(std::move(ns)));
}


// TgBTCMintEvent 0x77A80EF3: coins amount, MsgAddress recipient, uint256 txid.
td::Result<Value> parse_tgbtc_mint_event(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32) || !cs.advance(32)) {
    return td::Status::Error("tgbtc mint: opcode underflow");
  }
  auto parsed = parse_mint_log(cs);
  if (!parsed.ok) {
    return td::Status::Error("tgbtc mint: invalid log");
  }
  Value::Fields ns;
  ns.emplace_back("amount", Value::make_int(std::move(parsed.amount)));
  ns.emplace_back("recipient_address", std::move(parsed.recipient));
  ns.emplace_back("bitcoin_txid", Value::make_int(std::move(parsed.txid)));
  return Value::make_dict(std::move(ns));
}

// Two addresses parsed tolerantly. On the first address-load error, the
// already-parsed address stays set and the rest remain addr_none (partial
// results retained deliberately; no atomic rollback).
td::Result<Value> parse_tgbtc_burn_event(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32) || !cs.advance(32)) {
    return td::Status::Error("tgbtc burn: opcode underflow");
  }
  TRY_RESULT(amount, load_coins_py(cs));
  Value sender = Value::make_account_none();
  Value pegout = Value::make_account_none();
  auto r_sender = load_address_py(cs);
  if (r_sender.is_ok()) {
    sender = account_id_or_none(r_sender.ok());
    auto r_pegout = load_address_py(cs);
    if (r_pegout.is_ok()) {
      pegout = account_id_or_none(r_pegout.ok());
    }
  }
  Value::Fields ns;
  ns.emplace_back("amount", Value::make_int(std::move(amount)));
  ns.emplace_back("sender_address", std::move(sender));
  ns.emplace_back("pegout_address", std::move(pegout));
  return Value::make_dict(std::move(ns));
}

// TgBTCNewKeyEvent 0x27756729: coins amount, uint256 pubkey, MsgAddress pegout.
td::Result<Value> parse_tgbtc_new_key_event(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32) || !cs.advance(32)) {
    return td::Status::Error("tgbtc new key: opcode underflow");
  }
  TRY_RESULT(amount, load_coins_py(cs));
  auto pubkey = cs.have(256) ? cs.fetch_int256(256, false) : td::RefInt256{};
  if (pubkey.is_null()) {
    return td::Status::Error("tgbtc new key: pubkey underflow");
  }
  TRY_RESULT(address, load_address_py(cs));
  bool pegout_ok = true;
  Value pegout = account_id_of(address, pegout_ok);
  if (!pegout_ok) {
    return td::Status::Error("tgbtc new key: invalid pegout address");
  }
  Value::Fields ns;
  ns.emplace_back("amount", Value::make_int(std::move(amount)));
  ns.emplace_back("new_internal_pubkey", Value::make_int(std::move(pubkey)));
  ns.emplace_back("pegout_address", std::move(pegout));
  return Value::make_dict(std::move(ns));
}

// TgBTCDkgCompletedEvent 0x453443A6: u64 timestamp, uint256 pubkey.
td::Result<Value> parse_tgbtc_dkg_completed_event(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32) || !cs.advance(32)) {
    return td::Status::Error("tgbtc dkg: opcode underflow");
  }
  if (!cs.have(64)) {
    return td::Status::Error("tgbtc dkg: timestamp underflow");
  }
  td::RefInt256 timestamp = refint_u64(cs.fetch_ulong(64));
  auto pubkey = cs.have(256) ? cs.fetch_int256(256, false) : td::RefInt256{};
  if (pubkey.is_null()) {
    return td::Status::Error("tgbtc dkg: pubkey underflow");
  }
  Value::Fields ns;
  ns.emplace_back("timestamp", Value::make_int(std::move(timestamp)));
  ns.emplace_back("internal_pubkey", Value::make_int(std::move(pubkey)));
  return Value::make_dict(std::move(ns));
}

// Merge every ContractDeploy in the consumed set's children into the
// produced block (after wrapper-merge).
void tgbtc_deploy_absorb(Block *produced, const ShaperMatch &m) {
  std::vector<Block *> deploys;
  for (Block *b : m.consumed) {
    for (Block *c : b->children_blocks) {
      if (c->btype == mch::btype::kContractDeploy) {
        deploys.push_back(c);
      }
    }
  }
  if (!deploys.empty()) {
    produced->merge_blocks(deploys);
  }
}

}  // namespace mch
