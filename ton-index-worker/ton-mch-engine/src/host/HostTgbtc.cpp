// tgBTC host fn + shaper (builders/tgbtc.py + blocks/tgbtc.py). See
// host/HostImpls.h for the internal registry surface.
//
// Consumed-set-driven: head = sole 0x3F781D24 call, success log = sole
// 0x77A80EF3 call, jetton_mint = sole `jetton_mint` block. The jetton_mint
// block is builder-tier (not produced under the IR-only engine today), so this
// fn rejects on the current corpus — but it is a faithful port for when that
// block is available (and both twins reject identically meanwhile).
#include "host/HostImpls.h"

#include "host/HostCommon.h"

#include "BlockTree.h"
#include "BuildRuntime.h"
#include "HostRegistry.h"
#include "MsgParse.h"
#include "parse/PSlice.h"

#include "common/bigint.hpp"
#include "common/refint.h"
#include "vm/cellslice.h"

#include <string>
#include <vector>

namespace mch {

namespace {

constexpr std::uint32_t kTgbtcMintHead = 0x3F781D24;
constexpr std::uint32_t kTgbtcMintSuccessLog = 0x77A80EF3;

const Block *first_call_op(const std::vector<const Block *> &consumed, std::uint32_t op) {
  for (const Block *b : consumed) {
    if (is_call_op(b, op)) {
      return b;
    }
  }
  return nullptr;
}

// int(txid).to_bytes(32, "little").hex(): 32-byte little-endian, lowercase hex.
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

}  // namespace

// builders/tgbtc.py tgbtc_mint_data(consumed): the 9-field namespace, or Null
// to reject (missing head/log/mint or unparseable log).
EvalResult tgbtc_mint_data(BuildEnv &, const std::vector<Value> &args) {
  if (args.size() != 1 || args[0].t != VType::List) {
    return rt_fault("tgbtc_mint_data: bad arguments");
  }
  std::vector<const Block *> consumed;
  for (const Value &v : *args[0].items) {
    const Block *b = as_block(v);
    if (b != nullptr) {
      consumed.push_back(b);
    }
  }
  const Block *head = first_call_op(consumed, kTgbtcMintHead);
  const Block *success_log = first_call_op(consumed, kTgbtcMintSuccessLog);
  const Block *jetton_mint = nullptr;
  for (const Block *b : consumed) {
    if (b->btype == "jetton_mint") {
      jetton_mint = b;
      break;
    }
  }
  if (head == nullptr || success_log == nullptr || jetton_mint == nullptr) {
    return rt_ok(Value::null());  // core: missing piece -> reject
  }

  const Message *hmsg = block_msg(head);
  Value sender = account_from_opt(hmsg != nullptr ? hmsg->source : std::nullopt);

  // TgBTCMintEvent(success_log body): the legacy `success` flag is set right
  // after the log parse; a later failure (jetton_mint field access) keeps the
  // already-built fields. Mirror by treating a parse failure as reject.
  auto r_body = block_body(success_log);
  if (r_body.is_error()) {
    return rt_ok(Value::null());  // success stays False -> None
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
  auto r_amt = load_coins_py(cs);
  td::RefInt256 amount = r_amt.is_ok() ? r_amt.move_as_ok() : td::make_refint(0);
  auto r_rcpt = load_address_py(cs);
  if (r_rcpt.is_error()) {
    return rt_ok(Value::null());
  }
  Value recipient = r_rcpt.move_as_ok();
  if (!cs.have(256)) {
    return rt_ok(Value::null());
  }
  auto txid = cs.fetch_int256(256, false);
  if (txid.is_null()) {
    return rt_ok(Value::null());
  }

  const Message *lmsg = block_msg(success_log);
  Value teleport_contract = account_from_opt(lmsg != nullptr ? lmsg->source : std::nullopt);
  std::string bitcoin_txid = txid_le_hex(txid);

  // jetton_mint.data['asset'] + AccountId(jetton_mint.data['to_jetton_wallet']).
  Value asset = data_field(jetton_mint, "asset");
  Value to_wallet = data_field(jetton_mint, "to_jetton_wallet");
  Value recipient_wallet =
      to_wallet.t == VType::Str ? account_from_opt(std::optional<std::string>(to_wallet.str))
      : to_wallet.t == VType::Account ? to_wallet
                                      : Value::make_account_none();

  Value::Fields ns;
  ns.emplace_back("sender", std::move(sender));
  ns.emplace_back("recipient", std::move(recipient));
  ns.emplace_back("amount", Value::make_int(std::move(amount)));
  ns.emplace_back("asset", std::move(asset));
  ns.emplace_back("bitcoin_txid", Value::make_str(std::move(bitcoin_txid)));
  ns.emplace_back("success", Value::make_bool(true));
  ns.emplace_back("recipient_wallet", std::move(recipient_wallet));
  ns.emplace_back("teleport_contract", std::move(teleport_contract));
  ns.emplace_back("crippled", Value::make_bool(false));
  return rt_ok(Value::make_obj(std::move(ns)));
}

// builders/tgbtc.py tgbtc_deploy_absorb shaper: merge every ContractDeploy in
// the consumed set's children_blocks into the produced block (post wrapper-merge).
void tgbtc_deploy_absorb(Block *produced, const ShaperMatch &m) {
  std::vector<Block *> deploys;
  for (Block *b : m.consumed) {
    for (Block *c : b->children_blocks) {
      if (c->btype == "contract_deploy") {
        deploys.push_back(c);
      }
    }
  }
  if (!deploys.empty()) {
    produced->merge_blocks(deploys);
  }
}

}  // namespace mch
