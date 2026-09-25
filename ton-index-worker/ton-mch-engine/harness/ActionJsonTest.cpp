#include "ActionJsonTest.h"

#include "ActionJson.h"
#include "ClassifyCore.h"
#include "GenMatchers.h"
#include "fixtures/FixtureLoader.h"
#include "fixtures/FixtureLookupSource.h"
#include "fixtures/IrJson.h"

#include "td/utils/JsonBuilder.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>

namespace mch {
namespace {

using JsonType = td::JsonValue::Type;

void require(bool ok, const std::string &where, const std::string &reason) {
  if (!ok) throw std::runtime_error(where + ": " + reason);
}

td::JsonValue parse(std::string &json, const std::string &where) {
  auto result = td::json_decode(json);
  require(result.is_ok(), where, "invalid JSON");
  return result.move_as_ok();
}

void check_action(const Action &action, const ActionRowContext &ctx) {
  auto rows_json = action_rows_json({action}, ctx);
  auto rows = parse(rows_json, action.action_id);
  require(rows.type() == JsonType::Array && rows.get_array().size() == 1,
          action.action_id, "expected one actions row");
  const auto &row = rows.get_array().front();
  for (const auto *name : {"trace_id", "action_id", "start_lt", "end_lt", "start_utime", "end_utime",
                           "mc_seqno_end", "trace_end_lt", "trace_external_hash", "trace_external_hash_norm",
                           "trace_end_utime", "trace_mc_seqno_end", "type", "success", "tx_hashes",
                           "parent_action_id", "ancestor_type", "value_extra_currencies", "payload"}) {
    require(jfield(row, td::Slice(name)) != nullptr, action.action_id + "." + name, "missing row field");
  }
  const auto *currencies = jfield(row, "value_extra_currencies");
  require(currencies->type() == JsonType::Object, action.action_id + ".value_extra_currencies", "expected object");
  require(currencies->get_object().field_count() == action.value_extra_currencies.size(),
          action.action_id + ".value_extra_currencies", "wrong currency count");
  for (const auto &[id, amount] : action.value_extra_currencies) {
    require(jstr(*currencies, std::to_string(id)) == amount,
            action.action_id + ".value_extra_currencies." + std::to_string(id), "wrong amount");
  }
  auto accounts_json = action_account_rows_json(action, ctx);
  auto accounts = parse(accounts_json, action.action_id + ".action_accounts");
  require(accounts.type() == JsonType::Array, action.action_id, "expected account rows array");
  std::set<std::string> expected;
  for (const auto &account : action.accounts) expected.insert(canonicalize_or_passthrough(account));
  require(accounts.get_array().size() == expected.size(), action.action_id, "wrong deduplicated account count");
  for (const auto &account : accounts.get_array()) {
    require(account.get_object().field_count() == 8 && expected.erase(jstr(account, "account")) == 1 &&
                jstr(account, "action_id") == action.action_id && jstr(account, "trace_id") == ctx.trace_id &&
                jstr(account, "trace_end_lt") == std::to_string(ctx.trace_end_lt) &&
                jstr(account, "action_end_lt") == std::to_string(action.end_lt) &&
                jstr(account, "trace_end_utime") == std::to_string(ctx.trace_end_utime) &&
                jstr(account, "trace_mc_seqno_end") == std::to_string(ctx.trace_mc_seqno_end) &&
                jstr(account, "action_end_utime") == std::to_string(action.end_utime),
            action.action_id + ".action_accounts", "wrong account row");
  }
}

}  // namespace

int run_actions_json_test() {
  try {
    std::ifstream input(MCH_ACTION_FIXTURES);
    require(input.good(), MCH_ACTION_FIXTURES, "cannot open fixture manifest");
    std::string manifest_json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    auto manifest = parse(manifest_json, MCH_ACTION_FIXTURES);
    const auto *fixtures = jfield(manifest, "fixtures");
    require(fixtures != nullptr && fixtures->type() == JsonType::Object &&
                fixtures->get_object().field_count() != 0, MCH_ACTION_FIXTURES, "empty fixture manifest");
    const auto &matchers = gen_matchers_ir();
    const auto setup = prepare_classify(matchers);
    require(setup.error.empty(), "classify", setup.error);
    std::size_t row_count = 0;
    for (const auto &[id, meta] : fixtures->get_object().field_values_) {
      const auto path = std::string(MCH_ACTION_TRACES) + "/" + id.str() + ".lz4";
      auto loaded = load_trace_context(path);
      require(loaded.is_ok(), path, "cannot load fixture");
      auto ctx = loaded.move_as_ok();
      FixtureLookupSource source(&ctx.trace.interfaces, &ctx.trace.code_hashes);
      auto result = classify_trace(ctx, matchers, setup, source);
      const ActionRowContext context{ctx.trace.trace_id, std::nullopt, std::nullopt, ctx.trace.end_lt,
                                    static_cast<std::int32_t>(ctx.trace.end_utime),
                                    static_cast<std::int32_t>(ctx.trace.mc_seqno_end)};
      std::vector<Action> actions;
      for (const auto &row : result.failure ? result.fallback_rows : result.action_rows) {
        Action action;
        if (build_action(row, action)) actions.push_back(std::move(action));
      }
      if (!result.failure && result.unknown_trace) actions.push_back(create_unknown_action(ctx.trace));
      for (const auto &action : actions) check_action(action, context);
      auto batch_json = action_rows_json(actions, context);
      auto batch = parse(batch_json, id.str());
      require(batch.type() == JsonType::Array && batch.get_array().size() == actions.size(), id.str(), "wrong row array");
      for (std::size_t i = 0; i < actions.size(); ++i) {
        const auto &row = batch.get_array()[i];
        require(jfield(row, "payload") != nullptr, actions[i].action_id, "missing payload");
      }
      row_count += actions.size();
    }
    require(row_count != 0, "fixtures", "no actions checked");
    schema::Trace trace{};
    trace.trace_id.set_zero();
    trace.external_hash = trace.trace_id;
    trace.external_hash_norm = trace.trace_id;
    trace.end_lt = 123;
    trace.end_utime = 456;
    trace.mc_seqno_end = 789;
    const auto context = row_context(trace);
    require(context.trace_id == td::base64_encode(trace.trace_id.as_slice()) &&
                context.trace_external_hash == context.trace_id && context.trace_external_hash_norm == context.trace_id &&
                context.trace_end_lt == 123 && context.trace_end_utime == 456 && context.trace_mc_seqno_end == 789,
            "synthetic.trace", "wrong row context");
    Action action;
    action.action_id = context.trace_id;
    action.type = "ton_transfer";
    action.end_lt = 12;
    action.end_utime = 34;
    action.value_extra_currencies = {{239, "1000"}, {240, "5"}};
    action.accounts = {"0:" + std::string(64, 'a'), "0:" + std::string(64, 'A')};
    check_action(action, context);
    action.data = Value::make_dict({{"comment", Value::make_str(std::string("a\0b", 3))}});
    auto comment_json = action_data_json(action);
    require(comment_json.find("\\u0000") == std::string::npos,
            action.action_id + ".data.comment", "JSON contains escaped NUL");
    auto comment_row = parse(comment_json, action.action_id);
    require(jstr(comment_row, "comment") == "ab",
            action.action_id + ".data.comment", "NUL must be stripped");
    action.data = Value::make_dict({{"parent_gasless_action", Value::make_str(context.trace_id)}});
    auto extra_json = action_data_json(action);
    auto extra_row = parse(extra_json, action.action_id);
    require(jstr(extra_row, "parent_gasless_action") == context.trace_id,
            action.action_id + ".data", "populated data must round-trip exactly once");
    std::printf("PASS actions_json: %zu fixtures, %zu actions, extra currencies\n",
                fixtures->get_object().field_count(), row_count);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL actions_json: %s\n", error.what());
    return 1;
  }
}

}  // namespace mch
