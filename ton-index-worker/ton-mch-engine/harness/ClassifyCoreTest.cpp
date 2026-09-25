// Classify-core gate: topology guards, fallback and owned schema outcomes.
#include "ClassifyCoreTest.h"

#include "BlockTree.h"
#include "CelldbTier2Lookup.h"
#include "ClassifyCore.h"
#include "ExprRuntime.h"
#include "EnginePrep.h"
#include "GenMatchers.h"
#include "HostRegistry.h"
#include "MsgParse.h"
#include "TieredLookupSource.h"
#include "IrTables.h"
#include "TraceModel.h"
#include "fixtures/FixtureLookupSource.h"

#include "td/utils/base64.h"
#include "vm/boc.h"
#include "vm/cells/CellBuilder.h"
#include "vm/excno.hpp"
#include <msgpack.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace mch {
namespace {

int g_fail = 0;

void check(const std::string &name, bool ok) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
  if (!ok) g_fail++;
}

enum class HostThrow { vm_error, std_error, bad_alloc, no_gas, virt_error, unknown };
HostThrow g_serious_kind = HostThrow::bad_alloc;

EvalResult throwing_test_host(BuildEnv &, const std::vector<Value> &) {
  switch (g_serious_kind) {
    case HostThrow::vm_error: throw vm::VmError(vm::Excno::cell_und, "host vm error");
    case HostThrow::std_error: throw std::runtime_error("host standard error");
    case HostThrow::bad_alloc: throw std::bad_alloc();
    case HostThrow::no_gas: throw vm::VmNoGas();
    case HostThrow::virt_error: throw vm::VmVirtError();
    case HostThrow::unknown: throw 7;
  }
  return EvalResult{false, Value::null(), {}};
}

TraceContext make_host_test_context(const std::string &trace_id) {
  TraceContext ctx;
  ctx.trace.trace_id = trace_id;
  auto tx = std::make_unique<Transaction>();
  tx->hash = trace_id + "-tx";
  tx->lt = 100;
  tx->now = 200;
  tx->mc_block_seqno = 300;
  tx->account = "0:" + std::string(64, 'B');
  tx->descr = "ord";
  tx->orig_status = "active";
  tx->end_status = "active";
  auto in = std::make_unique<Message>();
  in->msg_hash = trace_id + "-message";
  in->tx_hash = tx->hash;
  in->tx_lt = tx->lt;
  in->direction = "in";
  in->source = "0:" + std::string(64, 'A');
  in->destination = tx->account;
  in->opcode = 0x11223344;
  in->value = 1000;
  in->tx = tx.get();
  tx->messages.push_back(std::move(in));
  ctx.trace.transactions.push_back(std::move(tx));
  ctx.tree = to_tree(ctx.trace);
  ctx.root = init_block(ctx.arena, ctx.tree.root);
  return ctx;
}

EvalResult call_throwing_host(BuildEnv &env, HostThrow kind) {
  static const HostFnEntry entry{throwing_test_host, 1};
  g_serious_kind = kind;
  return rt_call_hostfn(env, "throwing_test_host", &entry, {Value::make_block(env.anchor)});
}

CompiledMatcher test_matcher(std::string name, int artifact_index) {
  CompiledMatcher matcher;
  matcher.name = std::move(name);
  matcher.artifact_index = artifact_index;
  matcher.produces = {"call_contract"};
  matcher.anchor_kind = AnchorKind::BType;
  matcher.anchor_btypes = {"call_contract"};
  CompiledNode node;
  node.kind = NodeKind::BlockType;
  node.btype = "call_contract";
  matcher.nodes.push_back(std::move(node));
  return matcher;
}

int g_host_reject_calls = 0;
int g_parse_reject_calls = 0;
int g_success_calls = 0;
bool g_rejected_anchor_stayed_unconsumed = true;

BuildOutcome host_reject_build(BuildEnv &env) {
  g_host_reject_calls++;
  g_rejected_anchor_stayed_unconsumed &= env.anchor->parent == nullptr;
  EvalResult result = call_throwing_host(env, HostThrow::std_error);
  return result.faulted ? BuildOutcome::rejected(result.message) : BuildOutcome::accepted();
}

BuildOutcome required_parse_reject_build(BuildEnv &env) {
  g_parse_reject_calls++;
  g_rejected_anchor_stayed_unconsumed &= env.anchor->parent == nullptr;
  const Value malformed = Value::make_cell(vm::CellBuilder().finalize());
  EvalResult required = rt_parse_expr(malformed, {"ChangeDnsRecordMessage"}, false);
  return required.faulted ? BuildOutcome::rejected(required.message) : BuildOutcome::accepted();
}

BuildOutcome successful_build(BuildEnv &env) {
  g_success_calls++;
  g_rejected_anchor_stayed_unconsumed &= env.anchor->parent == nullptr;
  return BuildOutcome::accepted();
}

bool g_bad_payload_address = true;
bool g_payload_bad_first = false;
int g_payload_build_calls = 0;
BuildOutcome payload_address_build(BuildEnv &) {
  bool bad = g_bad_payload_address || (g_payload_bad_first && g_payload_build_calls == 0);
  ++g_payload_build_calls;
  BuildOutcome out = BuildOutcome::accepted();
  out.data = Value::make_dict({
      {"response_address", Value::make_account_raw(bad ? "bad-address" :
                                                    "0:" + std::string(64, 'A'))},
      {"query_id", Value::make_int64(1)},
  });
  return out;
}

BuildOutcome negative_nft_index_build(BuildEnv &) {
  BuildOutcome out = BuildOutcome::accepted();
  out.data = Value::make_dict({
      {"new_owner", Value::make_account_raw("0:" + std::string(64, 'A'))},
      {"nft", Value::make_dict({{"index", Value::make_int64(-1)}})},
  });
  return out;
}

void test_payload_encode_failure_skips_one_action() {
  std::vector<CompiledMatcher> matchers{test_matcher("payload-encode", 0)};
  matchers.front().produces = {"jetton_transfer"};
  ClassifySetup setup;
  setup.included = {0};
  setup.build_fns.emplace(0, payload_address_build);
  MchEnginePrep prep{&matchers, std::move(setup)};

  schema::Transaction tx{};
  tx.hash.set_zero();
  tx.hash.data()[31] = 1;
  tx.account = block::StdAddress(0, tx.hash);
  tx.lt = 100;
  tx.now = 200;
  tx.mc_seqno = 300;
  tx.orig_status = schema::AccountStatus::active;
  tx.end_status = schema::AccountStatus::active;
  tx.description = schema::TransactionDescr_ord{};
  schema::Message message{};
  message.hash.set_zero();
  message.hash.data()[31] = 2;
  message.source = "0:" + std::string(64, 'B');
  message.destination = "0:" + tx.hash.to_hex();
  message.opcode = 0x11223344;
  message.value = schema::CurrencyCollection{td::make_refint(1000), {}};
  tx.in_msg = std::move(message);
  TieredLookupSource::InterfaceMap interfaces;
  TieredLookupSource source(&interfaces);
  g_bad_payload_address = true;
  g_payload_bad_first = false;
  g_payload_build_calls = 0;
  auto bad = classify_schema_trace(prep, "payload-bad", {tx}, source);
  check("payload_failure/outcome", bad.outcome == TraceOutcome::classified);
  check("payload_failure/reason", bad.failure_reason.find("response_destination expects W:HEX address: bad-address") != std::string::npos);
  check("payload_failure/unknown_row", bad.rows.size() == 1 &&
        bad.rows.front().type == "unknown" && bad.unknown_row);
  check("payload_failure/skipped_one", bad.skipped_payload_actions == 1);

  schema::Transaction child = tx;
  child.hash.data()[31] = 3;
  child.account = block::StdAddress(0, child.hash);
  child.lt = 101;
  schema::Message link = *tx.in_msg;
  link.hash.data()[31] = 4;
  link.source = "0:" + tx.hash.to_hex();
  link.destination = "0:" + child.hash.to_hex();
  tx.out_msgs.push_back(link);
  child.in_msg = link;
  g_bad_payload_address = false;
  g_payload_bad_first = true;
  g_payload_build_calls = 0;
  auto mixed = classify_schema_trace(prep, "payload-mixed", {tx, child}, source);
  check("payload_mixed/two_actions_built", g_payload_build_calls == 2);
  check("payload_mixed/classified", mixed.outcome == TraceOutcome::classified);
  check("payload_mixed/one_good_row", mixed.rows.size() == 1);
  check("payload_mixed/one_skipped", mixed.skipped_payload_actions == 1);
  check("payload_mixed/reason", mixed.failure_reason.find("response_destination expects W:HEX address: bad-address") != std::string::npos);
  g_payload_bad_first = false;
  g_payload_build_calls = 0;
  auto good = classify_schema_trace(prep, "payload-good", {tx}, source);
  check("payload_failure/process_continues", good.outcome == TraceOutcome::classified &&
        !good.rows.empty() && good.skipped_payload_actions == 0 && good.failure_reason.empty());

  std::vector<CompiledMatcher> nft_matchers{test_matcher("negative-nft-index", 0)};
  nft_matchers.front().produces = {"nft_transfer"};
  ClassifySetup nft_setup;
  nft_setup.included = {0};
  nft_setup.build_fns.emplace(0, negative_nft_index_build);
  MchEnginePrep nft_prep{&nft_matchers, std::move(nft_setup)};
  auto nft_result = classify_schema_trace(nft_prep, "negative-nft-index", {tx}, source);
  check("nft_index/negative/not_skipped", nft_result.skipped_payload_actions == 0 &&
        nft_result.rows.size() == 1 && nft_result.rows.front().type == "nft_transfer");
  if (nft_result.rows.size() == 1 && nft_result.rows.front().type == "nft_transfer") {
    const auto &payload = nft_result.rows.front().encoded_payload;
    auto obj = msgpack::unpack(payload.data() + 1, payload.size() - 1).get();
    bool round_trip = false;
    if (obj.type == msgpack::type::MAP) {
      for (std::uint32_t i = 0; i < obj.via.map.size; ++i) {
        const auto &entry = obj.via.map.ptr[i];
        if (entry.key.as<int>() == 67) round_trip = entry.val.as<std::string>() == "-1";
      }
    }
    check("nft_index/negative/payload_round_trip", round_trip);
  }
}

const std::string kUncoveredWorkchainAddress = "1:" + std::string(64, 'A');

BuildOutcome uncovered_workchain_lookup_build(BuildEnv &env) {
  EvalResult lookup = rt_lookup_build(
      env, "nominator_pool", {Value::make_str(kUncoveredWorkchainAddress)});
  if (lookup.faulted) return BuildOutcome::rejected(lookup.message);
  return lookup.value.is_null() ? BuildOutcome::rejected("nominator pool lookup missed")
                                : BuildOutcome::accepted();
}

const std::string kTelemintLookupAddress = "0:" + std::string(64, 'A');

BuildOutcome telemint_lookup_build(BuildEnv &env) {
  EvalResult lookup = rt_lookup_build(
      env, "telemint_item", {Value::make_str(kTelemintLookupAddress)});
  if (lookup.faulted) return BuildOutcome::rejected(lookup.message);
  return lookup.value.is_null() ? BuildOutcome::rejected("telemint item lookup missed")
                                : BuildOutcome::accepted();
}

BuildOutcome serious_host_build(BuildEnv &env) {
  EvalResult result = call_throwing_host(env, g_serious_kind);
  return result.faulted ? BuildOutcome::rejected(result.message) : BuildOutcome::accepted();
}

void test_host_fn_exception_is_clean_rejection() {
  TraceContext ctx = make_host_test_context("host-clean-rejection");
  BuildEnv env;
  env.anchor = ctx.root;
  for (HostThrow kind : {HostThrow::vm_error, HostThrow::std_error}) {
    const std::string name = kind == HostThrow::vm_error ? "vm_error" : "std_error";
    bool threw = false;
    bool faulted = false;
    try {
      faulted = call_throwing_host(env, kind).faulted;
    } catch (...) {
      threw = true;
    }
    check("host_fn/clean_rejection/" + name + "/no_throw", !threw);
    check("host_fn/clean_rejection/" + name + "/faulted", faulted);
  }
}

void test_host_fn_error_rejects_attempt_and_next_matcher_runs() {
  TraceContext ctx = make_host_test_context("host-rejection-continuation");
  std::vector<CompiledMatcher> matchers{
      test_matcher("host-error", 0),
      test_matcher("required-parse-error", 1),
      test_matcher("success", 2),
  };
  ClassifySetup setup;
  setup.included = {0, 1, 2};
  setup.build_fns.emplace(0, host_reject_build);
  setup.build_fns.emplace(1, required_parse_reject_build);
  setup.build_fns.emplace(2, successful_build);
  FixtureLookupSource src(&ctx.trace.interfaces, &ctx.trace.code_hashes);
  g_host_reject_calls = 0;
  g_parse_reject_calls = 0;
  g_success_calls = 0;
  g_rejected_anchor_stayed_unconsumed = true;

  ClassifyResult result = classify_trace(ctx, matchers, setup, src);
  check("host_fn/continuation/no_trace_failure", !result.failure);
  check("host_fn/continuation/no_fallback", result.fallback_rows.empty());
  check("host_fn/continuation/host_attempted", g_host_reject_calls > 0);
  check("host_fn/continuation/required_parse_attempted", g_parse_reject_calls == 1);
  check("host_fn/continuation/later_matcher_ran", g_success_calls == 1);
  check("host_fn/continuation/rejections_did_not_consume",
        g_rejected_anchor_stayed_unconsumed);
  check("host_fn/continuation/success_action_preserved",
        result.actions.size() == 1 && result.actions.front().matcher_name == "success" &&
            result.actions.front().anchor == ctx.root &&
            ctx.root->parent == result.actions.front().produced);
}

void test_host_fn_serious_exception_stays_trace_failure() {
  struct Case {
    const char *name;
    HostThrow kind;
  };
  const std::vector<Case> cases{{"bad_alloc", HostThrow::bad_alloc},
                                {"no_gas", HostThrow::no_gas},
                                {"virt_error", HostThrow::virt_error},
                                {"unknown", HostThrow::unknown}};
  for (const Case &test : cases) {
    TraceContext ctx = make_host_test_context(std::string("host-classify-") + test.name);
    std::vector<CompiledMatcher> matchers{test_matcher("serious-host-error", 0)};
    ClassifySetup setup;
    setup.included = {0};
    g_serious_kind = test.kind;
    setup.build_fns.emplace(0, serious_host_build);
    FixtureLookupSource src(&ctx.trace.interfaces, &ctx.trace.code_hashes);
    ClassifyResult result = classify_trace(ctx, matchers, setup, src);
    check(std::string("host_fn/serious/") + test.name + "/trace_failed", result.failure);
    check(std::string("host_fn/serious/") + test.name + "/engine_fault",
          result.failure_category == FailureCategory::engine_fault);
    check(std::string("host_fn/serious/") + test.name + "/actions_discarded",
          result.actions.empty() && result.action_rows.empty());
    check(std::string("host_fn/serious/") + test.name + "/fallback_preserved",
          !result.fallback_rows.empty());
  }
}

const std::string kAuction = "0:" + std::string(64, 'A');
const std::string kNft = "0:" + std::string(64, 'B');
const std::string kBidder = "0:" + std::string(64, 'C');
const std::string kOwner = "0:" + std::string(64, 'D');
const std::string kMarketplace = "0:" + std::string(64, 'E');
const std::string kCollection = "0:" + std::string(64, 'F');
// Telegram Usernames, the collection a Fragment content URI is metadata for.
const std::string kFragmentCollection = "0:80D78A35F955A14B679FAA887FF4CD5BFC0F43B4A4EEA2A7E6927F3701B273C2";
const std::string kV1 = "/ACindAgW83MDT/7nKOMw8jBWexg2KpUMkCpLxBZLUA=";
const std::string kV2 = "ZmiHL6eXBUQ//UdSPo6eqfdquZ+aC1nSfej4GhwnudQ=";
const std::string kV3r2 = "G9nFo5v/t6DzQViLXdkrgTqEK/Ze8UEJOCIAzq+Pct8=";
const std::string kV3r3 = "u29ireD+stefqzuK6/CTCvmFU99gCTsgJ/Covxab/Ow=";
const std::string kV4r1 = "zlp4U06qps7tja/UhtB262CpsNbb+1Nnb2YmScBomVY=";

std::string boc_base64(const td::Ref<vm::Cell> &cell) {
  auto serialized = vm::std_boc_serialize(cell);
  return serialized.is_ok() ? td::base64_encode(td::Slice(serialized.ok())) : std::string{};
}

td::Ref<vm::Cell> comment_body(const std::string &comment) {
  return vm::CellBuilder().store_zeroes(32).store_bytes(comment).finalize();
}

Transaction *add_transaction(TraceContext &ctx, const std::string &name, std::int64_t lt,
                             const std::string &account, const std::string &source,
                             const std::string &destination, std::optional<std::uint32_t> opcode,
                             std::int64_t value, bool failed = false,
                             const td::Ref<vm::Cell> &body = {}) {
  auto tx = std::make_unique<Transaction>();
  tx->hash = name + "-tx";
  tx->lt = lt;
  tx->now = 200 + lt;
  tx->mc_block_seqno = 300;
  tx->account = account;
  tx->descr = "ord";
  tx->orig_status = "active";
  tx->end_status = "active";
  tx->aborted = failed;
  auto in = std::make_unique<Message>();
  in->msg_hash = name + "-message";
  in->tx_hash = tx->hash;
  in->tx_lt = lt;
  in->direction = "in";
  in->source = source;
  in->destination = destination;
  if (opcode) in->opcode = static_cast<std::int32_t>(*opcode);
  in->value = value;
  in->bounce = true;
  if (body.not_null()) in->content = MsgContent{name + "-body", boc_base64(body)};
  in->tx = tx.get();
  tx->messages.push_back(std::move(in));
  Transaction *out = tx.get();
  ctx.trace.transactions.push_back(std::move(tx));
  return out;
}

void link_child(Transaction *parent, Transaction *child) {
  auto out = std::make_unique<Message>();
  out->msg_hash = child->messages.front()->msg_hash;
  out->tx_hash = parent->hash;
  out->tx_lt = parent->lt;
  out->created_lt = child->lt;
  out->direction = "out";
  out->source = parent->account;
  out->destination = child->account;
  out->tx = parent;
  parent->messages.push_back(std::move(out));
}

// The content URI is display metadata; the teleitem gate reads the item's
// Telemint interface, which Fragment fixtures carry unless `telemint` says otherwise.
void add_nft_item(TraceContext &ctx, const std::string &address, bool fragment = false,
                  bool dns = false, const std::string &collection = kCollection,
                  std::optional<bool> telemint = std::nullopt) {
  Value::Fields content{{"uri", Value::make_str(fragment ? "https://nft.fragment.com/item" : "")}};
  Value::Fields item{{"address", Value::make_str(address)},
                     {"init", Value::make_bool(true)},
                     {"index", Value::make_int64(42)},
                     {"collection_address", Value::make_str(collection)},
                     {"owner_address", Value::make_str(kOwner)},
                     {"content", Value::make_dict(std::move(content))},
                     {"code_hash", Value::make_str(dns ? "i1/8nr/TkGTY1fVuRlnIJrt1k5I/XKSHKL5NYK9vUfk=" : "")}};
  Value::Fields ifaces{{"NftItem", Value::make_dict(std::move(item))}};
  if (telemint.value_or(fragment)) {
    ifaces.emplace_back("Telemint", Value::make_dict(Value::Fields{}));
  }
  ctx.trace.interfaces[address] = Value::make_dict(std::move(ifaces));
}

void add_nft_auction(TraceContext &ctx, const std::string &code_hash) {
  Value::Fields auction{{"mp_addr", Value::make_str(kMarketplace)},
                        {"nft_addr", Value::make_str(kNft)},
                        {"last_bid", Value::make_int64(0)},
                        {"nft_owner", Value::make_str(kOwner)},
                        {"code_hash", Value::make_str(code_hash)}};
  ctx.trace.interfaces[kAuction] =
      Value::make_dict({{"NftAuction", Value::make_dict(std::move(auction))}});
  add_nft_item(ctx, kNft);
}

void finish_trace(TraceContext &ctx, const std::string &name) {
  ctx.trace.trace_id = name;
  fill_trace_aggregates(ctx.trace);
  ctx.tree = to_tree(ctx.trace);
  ctx.root = init_block(ctx.arena, ctx.tree.root);
}

ClassifyResult classify_production(TraceContext &ctx) {
  const auto &matchers = gen_matchers_ir();
  const auto setup = prepare_classify(matchers);
  FixtureLookupSource source(&ctx.trace.interfaces, &ctx.trace.code_hashes);
  return classify_trace(ctx, matchers, setup, source);
}

Block *produced_by(const ClassifyResult &result, const std::string &name) {
  for (const CoreAction &action : result.actions) {
    if (action.matcher_name == name) return action.produced;
  }
  return nullptr;
}

std::size_t produced_count(const ClassifyResult &result, const std::string &name) {
  std::size_t count = 0;
  for (const CoreAction &action : result.actions) count += action.matcher_name == name;
  return count;
}

bool field_str(const Block *block, const char *name, const std::string &want) {
  const Value *value = block != nullptr ? block->data.field(name) : nullptr;
  return value != nullptr && (value->t == VType::Str || value->t == VType::Account) &&
         value->str == want;
}

bool field_num(const Block *block, const char *name, std::int64_t want) {
  const Value *value = block != nullptr ? block->data.field(name) : nullptr;
  return value != nullptr && value->num.not_null() &&
         value->num->to_dec_string() == std::to_string(want);
}

void test_auction_bid_raw_attempts() {
  struct Case {
    const char *name;
    const std::string *code_hash;
    std::optional<std::uint32_t> opcode;
    std::int64_t value;
    bool failed;
  };
  const std::vector<Case> cases{{"v1_low", &kV1, std::nullopt, 1, false},
                                {"v2_failed", &kV2, std::nullopt, 2, true},
                                {"v3r2", &kV3r2, 0x12345678, 3, false},
                                {"v3r3", &kV3r3, 3, 4, false},
                                {"v4r1_plain", &kV4r1, std::nullopt, 5, false},
                                {"v4r1_zero", &kV4r1, 0, 6, false},
                                {"v4r1_bid", &kV4r1, 0x344EF025, 7, false},
                                {"v4r1_fund", &kV4r1, 0x9AF07447, 8, false}};
  for (const Case &test : cases) {
    TraceContext ctx;
    add_transaction(ctx, test.name, 100, kAuction, kBidder, kAuction, test.opcode,
                    test.value, test.failed);
    add_nft_auction(ctx, *test.code_hash);
    finish_trace(ctx, test.name);
    ClassifyResult result = classify_production(ctx);
    Block *bid = produced_by(result, "getgems_auction_bid");
    const std::string prefix = std::string("auction_bid/raw/") + test.name;
    check(prefix + "/classified", !result.failure && bid != nullptr);
    check(prefix + "/fields",
          field_num(bid, "amount", test.value) && field_str(bid, "bidder", kBidder) &&
              field_str(bid, "auction", kAuction) && field_str(bid, "nft_address", kNft) &&
              field_num(bid, "nft_item_index", 42) &&
              field_str(bid, "nft_collection", kCollection));
    check(prefix + "/failed", bid != nullptr && bid->failed == test.failed);
  }
}

void test_auction_bid_item_families_and_negatives() {
  for (const auto &[name, fragment, dns] :
       std::vector<std::tuple<const char *, bool, bool>>{
           {"dns", false, true}, {"fragment", true, false}}) {
    TraceContext ctx;
    add_transaction(ctx, name, 100, kNft, kBidder, kNft, 0, 10);
    add_nft_item(ctx, kNft, fragment, dns, fragment ? kFragmentCollection : kCollection);
    finish_trace(ctx, name);
    ClassifyResult result = classify_production(ctx);
    Block *bid = produced_by(result, "auction_bid");
    check(std::string("auction_bid/item/") + name, !result.failure && bid != nullptr);
  }

  struct Negative {
    const char *name;
    std::string hash;
    std::string source;
    std::optional<std::uint32_t> opcode;
  };
  const std::vector<Negative> negatives{{"wrong_family", "unknown", kBidder, std::nullopt},
                                        {"v1_owner", kV1, kOwner, std::nullopt},
                                        {"v3r3_admin", kV3r3, kBidder, 555},
                                        {"v4r1_body", kV4r1, kBidder, 0x12345678}};
  for (const Negative &test : negatives) {
    TraceContext ctx;
    add_transaction(ctx, test.name, 100, kAuction, test.source, kAuction, test.opcode, 10);
    add_nft_auction(ctx, test.hash);
    finish_trace(ctx, test.name);
    ClassifyResult result = classify_production(ctx);
    check(std::string("auction_bid/negative/") + test.name,
          !result.failure && produced_count(result, "auction_bid") +
                                 produced_count(result, "getgems_auction_bid") == 0);
  }

  // A Fragment content URI without a Telemint interface is not a teleitem.
  TraceContext ctx;
  add_transaction(ctx, "fragment_uri_foreign_collection", 100, kNft, kBidder, kNft, 0, 10);
  add_nft_item(ctx, kNft, true, false, kCollection, /*telemint=*/false);
  finish_trace(ctx, "fragment_uri_foreign_collection");
  ClassifyResult result = classify_production(ctx);
  check("auction_bid/item/fragment_uri_foreign_collection",
        !result.failure && produced_count(result, "auction_bid") +
                               produced_count(result, "getgems_auction_bid") == 0);
}

void add_outbid_child(TraceContext &ctx, Transaction *parent, const std::string &name,
                      std::int64_t lt, bool fragment) {
  const auto body = fragment ? td::Ref<vm::Cell>{}
                             : comment_body("Your bid has been outbid by another user");
  Transaction *child = add_transaction(ctx, name, lt, kOwner, fragment ? kNft : kAuction,
                                       kOwner, fragment ? std::optional<std::uint32_t>{0x557CEA20}
                                                        : std::nullopt,
                                       7, false, body);
  link_child(parent, child);
}

void test_auction_outbid_attempts() {
  for (bool fragment : {false, true}) {
    const std::string family = fragment ? "fragment" : "getgems";
    TraceContext ctx;
    Transaction *root = add_transaction(ctx, family + "_bid", 100,
                                        fragment ? kNft : kAuction, kBidder,
                                        fragment ? kNft : kAuction, std::nullopt, 10);
    add_outbid_child(ctx, root, family + "_refund_1", 110, fragment);
    if (fragment) add_nft_item(ctx, kNft, true, false, kFragmentCollection);
    else add_nft_auction(ctx, kV2);
    finish_trace(ctx, family + "_outbid");
    ClassifyResult result = classify_production(ctx);
    Block *bid = produced_by(result, fragment ? "auction_bid" : "getgems_auction_bid");
    Block *outbid = produced_by(result, "auction_outbid");
    const std::string prefix = "auction_outbid/" + family;
    check(prefix + "/one", !result.failure && produced_count(result, "auction_outbid") == 1);
    check(prefix + "/bid_top_level", bid != nullptr && outbid != nullptr && bid->parent == nullptr);
  }
}

void test_ext_addr_str_pruned_cell_is_clean_rejection() {
  auto data = vm::CellBuilder().store_long(0xabcd, 16).finalize();
  auto pruned = vm::CellBuilder::do_create_pruned_branch(data, 1)->virtualize(0);
  const Value pruned_addr = Value::make_obj({
      {"bits", Value::make_int64(16)}, {"value", Value::make_cell(std::move(pruned))}});
  bool threw = false;
  bool faulted = false;
  try {
    faulted = rt_builtin_ext_addr_str(pruned_addr).faulted;
  } catch (...) {
    threw = true;
  }
  check("builtin/ext_addr_str/pruned/no_throw", !threw);
  check("builtin/ext_addr_str/pruned/faulted", faulted);

  const Value plain_addr = Value::make_obj({
      {"bits", Value::make_int64(16)},
      {"value", Value::make_cell(vm::CellBuilder().store_long(0xabcd, 16).finalize())}});
  EvalResult rendered = rt_builtin_ext_addr_str(plain_addr);
  check("builtin/ext_addr_str/plain/rendered",
        !rendered.faulted && rendered.value.t == VType::Str && rendered.value.str == "16;abcd");
}

void test_out_messages_no_body_hash() {
  auto tx = std::make_unique<Transaction>();
  auto in = std::make_unique<Message>();
  in->direction = "in";
  in->tx = tx.get();
  Message *in_ptr = in.get();
  tx->messages.push_back(std::move(in));
  auto out = std::make_unique<Message>();
  out->direction = "out";
  out->tx = tx.get();
  tx->messages.push_back(std::move(out));

  EventNode node;
  node.msg = in_ptr;
  node.tx = tx.get();
  Block block;
  block.event_nodes.push_back(&node);
  EvalResult r = rt_builtin_out_messages(Value::make_block(&block));
  check("builtin/out_messages/no_body/ok",
        !r.faulted && r.value.t == VType::List && r.value.items && r.value.items->size() == 1);
  const Value *bh = r.value.items->front().field("body_hash");
  check("builtin/out_messages/no_body/hash_null", bh != nullptr && rt_is_null(*bh));
}


// P -> C (child) -> G (grandchild), with G also reachable as one of P's
// next_blocks. G's topmost parent is P, so compaction must drop the edge
// instead of turning it into a self-edge.
void test_compact_connections_drops_self_edge() {
  BlockArena arena;
  Block *p = arena.make("call_contract");
  Block *c = arena.make("call_contract");
  Block *g = arena.make("call_contract");
  p->children_blocks.push_back(c);
  c->parent = p;
  c->children_blocks.push_back(g);
  g->parent = c;
  p->next_blocks.push_back(g);

  p->compact_connections();
  bool self_edge = false;
  for (const Block *n : p->next_blocks) {
    self_edge = self_edge || n == p;
  }
  check("block_tree/compact_connections/no_self_edge", !self_edge);
  check("block_tree/compact_connections/grandchild_edge_dropped", p->next_blocks.empty());
}

// X (self-edge) -> Y -> Z, merging {X, Y}. find_frontier walks X's next_blocks,
// so the self-edge would re-queue X forever; the frontier is still {Z}. Reached
// through merge_blocks because find_frontier is private.
void test_find_frontier_terminates_on_self_edge() {
  BlockArena arena;
  Block *x = arena.make("call_contract");
  Block *y = arena.make("call_contract");
  Block *z = arena.make("call_contract");
  y->min_lt_without_initiating_tx = 1;
  z->min_lt_without_initiating_tx = 2;
  x->connect(y);
  y->connect(z);
  x->next_blocks.push_back(x);

  Block *merged = arena.make("merged");
  check("block_tree/find_frontier/merge_accepted", merged->merge_blocks({x, y}));
  check("block_tree/find_frontier/frontier_is_outside_block",
        merged->next_blocks.size() == 1 && merged->next_blocks.front() == z);
  check("block_tree/find_frontier/frontier_reparented", z->previous_block == merged);
}

// A Pred anchor is never prefiltered away and no build fn is registered, so the
// build_fns lookup throws: the shortest route to the failure path.
ClassifyResult classify_with_forced_failure(TraceContext &ctx) {
  std::vector<CompiledMatcher> matchers(1);
  matchers[0].name = "forced-failure";
  matchers[0].anchor_kind = AnchorKind::Pred;
  ClassifySetup setup;
  setup.included = {0};
  FixtureLookupSource src(&ctx.trace.interfaces, &ctx.trace.code_hashes);
  return classify_trace(ctx, matchers, setup, src);
}

// A single-transaction trace that fails classification still owns its root: the
// basic fallback serializes it, so no unknown action is minted.
void test_childless_root_failure_serializes_root() {
  TraceContext ctx;
  ctx.trace.trace_id = "childless-root-failure";
  auto tx = std::make_unique<Transaction>();
  tx->hash = "childless-root-tx";
  tx->lt = 100;
  tx->now = 200;
  tx->mc_block_seqno = 300;
  tx->account = "0:" + std::string(64, 'B');
  tx->descr = "ord";
  tx->orig_status = "active";
  tx->end_status = "active";
  auto in = std::make_unique<Message>();
  in->msg_hash = "childless-root-message";
  in->tx_hash = tx->hash;
  in->tx_lt = tx->lt;
  in->direction = "in";
  in->source = "0:" + std::string(64, 'A');
  in->destination = tx->account;
  in->opcode = 0x11223344;
  in->value = 1000;
  in->tx = tx.get();
  tx->messages.push_back(std::move(in));
  ctx.trace.transactions.push_back(std::move(tx));
  ctx.tree = to_tree(ctx.trace);
  ctx.root = init_block(ctx.arena, ctx.tree.root);
  check("classify/childless_root_failure/no_children", ctx.tree.root->children.empty());

  ClassifyResult result = classify_with_forced_failure(ctx);
  check("classify/childless_root_failure/failed", result.failure);
  check("classify/childless_root_failure/one_fallback_row", result.fallback_rows.size() == 1);
  check("classify/childless_root_failure/row_is_the_root",
        result.fallback_rows.size() == 1 && result.fallback_rows.front().block != nullptr &&
            result.fallback_rows.front().block->anchor_tx_hash() == "childless-root-tx");
  check("classify/childless_root_failure/no_unknown_trace", !result.unknown_trace);
}

void test_schema_tick_tock() {
  const std::vector<CompiledMatcher> matchers;
  MchEnginePrep prep{&matchers, {}};
  TieredLookupSource src(nullptr);
  schema::Transaction root{};
  root.hash.set_zero();
  root.account = block::StdAddress(0, root.hash);
  root.description = schema::TransactionDescr_tick_tock{};
  auto lone = classify_schema_trace(prep, "tick-tock", {root}, src);
  check("schema/lone_tick_tock/skipped", lone.outcome == TraceOutcome::skipped_lone_tick_tock);
  check("schema/lone_tick_tock/no_rows", lone.rows.empty());

  schema::Transaction child = root;
  child.hash.data()[31] = 1;
  child.lt = 1;
  child.description = schema::TransactionDescr_ord{};
  schema::Message message{};
  message.hash = child.hash;
  message.source = "0:" + root.hash.to_hex();
  message.destination = message.source;
  child.in_msg = message;
  root.out_msgs.push_back(message);
  auto pair = classify_schema_trace(prep, "tick-tock-child", {root, child}, src);
  check("schema/tick_tock_with_child/not_skipped",
        pair.outcome != TraceOutcome::skipped_lone_tick_tock);
}

void test_tier2_uncovered_workchain_miss_preserves_trace() {
  std::vector<CompiledMatcher> matchers{
      test_matcher("uncovered-workchain-lookup", 0),
  };
  matchers.front().ref_lookups.insert("nominator_pool");
  ClassifySetup setup;
  setup.included = {0};
  setup.build_fns.emplace(0, uncovered_workchain_lookup_build);
  setup.kinds.insert("nominator_pool");
  MchEnginePrep prep{&matchers, std::move(setup)};

  schema::Transaction tx{};
  tx.hash.set_zero();
  tx.hash.data()[31] = 1;
  tx.account = block::StdAddress(0, tx.hash);
  tx.lt = 100;
  tx.now = 200;
  tx.mc_seqno = 300;
  tx.orig_status = schema::AccountStatus::active;
  tx.end_status = schema::AccountStatus::active;
  schema::TransactionDescr_ord descr{};
  schema::TrComputePhase_vm compute{};
  compute.exit_code = 0;
  descr.compute_ph = compute;
  tx.description = descr;
  schema::Message message{};
  message.hash.set_zero();
  message.hash.data()[31] = 2;
  message.source = "0:" + std::string(64, 'B');
  message.destination = "0:" + tx.hash.to_hex();
  message.opcode = 0x11223344;
  message.value = schema::CurrencyCollection{td::make_refint(1000), {}};
  message.bounce = true;
  tx.in_msg = std::move(message);

  AllShardStates states{vm::CellBuilder().finalize()};
  CelldbTier2Lookup tier2(&states, nullptr);
  int reads = 0;
  bool saw_uncovered_address = false;
  tier2.set_account_source(
      [&](const block::StdAddress &address)
          -> td::Result<std::optional<schema::AccountState>> {
        reads++;
        saw_uncovered_address = address.workchain == 1 &&
                                address.addr.to_hex() == std::string(64, 'A');
        return td::Status::Error("Account not found in shards");
      });
  TieredLookupSource source(
      nullptr, [&](const std::string &kind, const std::vector<Value> &args) {
        return tier2.fetch(kind, args);
      });

  auto result = classify_schema_trace(
      prep, "tier2-uncovered-workchain-miss", {tx}, source);
  check("schema/tier2_uncovered_workchain/looked_up_by_matcher",
        reads == 1 && saw_uncovered_address && source.stats().misses == 1);
  check("schema/tier2_uncovered_workchain/matcher_rejected",
        result.matcher_names.empty());
  check("schema/tier2_uncovered_workchain/outcome",
        result.outcome == TraceOutcome::classified);
  check("schema/tier2_uncovered_workchain/rows_retained", !result.rows.empty());
  check("schema/tier2_uncovered_workchain/writer_persists",
        should_persist_outcome(result.outcome));
  BlockClassification block;
  TraceClassificationResult trace;
  trace.outcome = result.outcome;
  block.traces.push_back(std::move(trace));
  check("schema/tier2_uncovered_workchain/task_can_replace",
        outcomes_allow_replacement(block));
}

void test_tier2_telemint_lookup_for_trace_reference_without_transaction() {
  std::vector<CompiledMatcher> matchers{test_matcher("telemint-tier2", 0)};
  matchers.front().ref_lookups.insert("telemint_item");
  ClassifySetup setup;
  setup.included = {0};
  setup.build_fns.emplace(0, telemint_lookup_build);
  setup.kinds.insert("telemint_item");
  MchEnginePrep prep{&matchers, std::move(setup)};

  schema::Transaction tx{};
  tx.hash.set_zero();
  tx.hash.data()[31] = 1;
  tx.account = block::StdAddress(0, tx.hash);
  tx.lt = 100;
  tx.now = 200;
  tx.mc_seqno = 300;
  tx.orig_status = schema::AccountStatus::active;
  tx.end_status = schema::AccountStatus::active;
  tx.description = schema::TransactionDescr_ord{};
  schema::Message out{};
  out.hash.set_zero();
  out.hash.data()[31] = 2;
  out.source = "0:" + tx.hash.to_hex();
  out.destination = kTelemintLookupAddress;
  out.opcode = 0x11223344;
  out.value = schema::CurrencyCollection{td::make_refint(1000), {}};
  tx.out_msgs.push_back(std::move(out));

  AllShardStates states{vm::CellBuilder().finalize()};
  CelldbTier2Lookup tier2(
      &states, nullptr,
      [](const block::StdAddress &address, const td::Ref<vm::Cell> &,
         const td::Ref<vm::Cell> &, const std::shared_ptr<block::ConfigInfo> &)
          -> td::Result<TelemintContract::Result> {
        TelemintContract::Result result{};
        result.address = address;
        return result;
      });
  int reads = 0;
  tier2.set_account_source([&](const block::StdAddress &address)
                               -> td::Result<std::optional<schema::AccountState>> {
    reads++;
    if (address.workchain != 0 || address.addr.to_hex() != std::string(64, 'A')) {
      return std::optional<schema::AccountState>{};
    }
    schema::AccountState state{};
    state.account = address;
    state.code = vm::CellBuilder().finalize();
    state.data = vm::CellBuilder().finalize();
    return std::optional<schema::AccountState>{std::move(state)};
  });
  TieredLookupSource::InterfaceMap tier1_interfaces;
  TieredLookupSource source(
      &tier1_interfaces, [&](const std::string &kind, const std::vector<Value> &args) {
        return tier2.fetch(kind, args);
      });

  auto result = classify_schema_trace(
      prep, "tier2-telemint-trace-reference-no-transaction", {tx}, source);
  check("schema/tier2_telemint/reference_is_message_destination",
        (tx.account.workchain != 0 || tx.account.addr.to_hex() != std::string(64, 'A')) &&
            tx.out_msgs.front().destination == kTelemintLookupAddress);
  const auto telemint_hits = source.stats().tier2_hits_by_kind.find("telemint_item");
  check("schema/tier2_telemint/detected_from_cell_db",
        reads == 1 && source.stats().tier2_hits == 1 &&
            telemint_hits != source.stats().tier2_hits_by_kind.end() && telemint_hits->second == 1);
  check("schema/tier2_telemint/matcher_accepted", result.matcher_names.size() == 1);
}

// Startup is fatal on a skipped matcher, so the compiled-in table must have none:
// a skip means a host name the engine does not provide.
void test_engine_prep_has_no_skips() {
  auto prepared = make_engine_prep();
  if (prepared.is_error()) {
    std::printf("prep error: %s\n", prepared.error().message().c_str());
  }
  check("prep/no_skips", prepared.is_ok());
}

}  // namespace

int run_classify_core_test() {
  g_fail = 0;
  test_payload_encode_failure_skips_one_action();
  test_host_fn_exception_is_clean_rejection();
  test_host_fn_error_rejects_attempt_and_next_matcher_runs();
  test_host_fn_serious_exception_stays_trace_failure();
  test_auction_bid_raw_attempts();
  test_auction_bid_item_families_and_negatives();
  test_auction_outbid_attempts();
  test_ext_addr_str_pruned_cell_is_clean_rejection();
  test_out_messages_no_body_hash();
  test_compact_connections_drops_self_edge();
  test_find_frontier_terminates_on_self_edge();
  test_childless_root_failure_serializes_root();
  test_schema_tick_tock();
  test_tier2_uncovered_workchain_miss_preserves_trace();
  test_tier2_telemint_lookup_for_trace_reference_without_transaction();
  test_engine_prep_has_no_skips();
  std::printf("CLASSIFY-CORE-TEST %s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
  return g_fail == 0 ? 0 : 1;
}

}  // namespace mch
