#include "ActionJsonTest.h"

#include "ActionJson.h"
#include "ClassifyCore.h"
#include "GenMatchers.h"
#include "fixtures/FixtureLoader.h"
#include "fixtures/FixtureLookupSource.h"
#include "fixtures/IrJson.h"

#include "td/utils/JsonBuilder.h"
#include "td/utils/base64.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace mch {
namespace {

using Ddl = std::map<std::string, std::map<std::string, std::string>>;
using JsonType = td::JsonValue::Type;

void require(bool ok, const std::string &where, const std::string &reason) {
  if (!ok) throw std::runtime_error(where + ": " + reason);
}

Ddl read_ddl() {
  std::ifstream input(MCH_ACTION_DDL);
  require(input.good(), MCH_ACTION_DDL, "cannot open DDL");
  Ddl ddl;
  std::string line, current;
  const std::regex composite(R"(create type ([a-z_][a-z_0-9]*) as\s*$)");
  const std::regex table(R"(create table if not exists (actions|action_accounts)\s*$)");
  while (std::getline(input, line)) {
    std::transform(line.begin(), line.end(), line.begin(), [](unsigned char c) { return std::tolower(c); });
    std::smatch match;
    if (std::regex_search(line, match, composite) || std::regex_search(line, match, table)) {
      current = match[1].str();
      ddl[current];
      continue;
    }
    if (current.empty()) continue;
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos || line[first] == '(') continue;
    if (line[first] == ')') {
      require(!ddl.at(current).empty(), current, "empty DDL definition");
      current.clear();
      continue;
    }
    if (line.compare(first, 11, "primary key") == 0) continue;
    std::istringstream fields(line);
    std::string name, type;
    fields >> name >> type;
    require(!name.empty() && !type.empty(), current, "unparsed DDL line: " + line);
    if (type.back() == ',') type.pop_back();
    require(ddl[current].emplace(name, type).second, current + "." + name, "duplicate DDL field");
  }
  require(current.empty(), current, "unterminated DDL definition");
  require(ddl.count("actions") && ddl.count("action_accounts"), MCH_ACTION_DDL, "missing row tables");
  return ddl;
}

std::set<std::string> unknown_fields;

void validate(const td::JsonValue &value, const std::string &type, const Ddl &ddl,
              const std::string &where) {
  if (value.type() == JsonType::Null) return;
  if (type.ends_with("[]")) {
    require(value.type() == JsonType::Array, where, "expected array for " + type);
    std::size_t i = 0;
    for (const auto &item : value.get_array()) {
      validate(item, type.substr(0, type.size() - 2), ddl, where + "[" + std::to_string(i++) + "]");
    }
    return;
  }
  if (auto composite = ddl.find(type); composite != ddl.end()) {
    require(value.type() == JsonType::Object, where, "expected object for " + type);
    for (const auto &[key, field] : value.get_object().field_values_) {
      const auto name = key.str();
      auto column = composite->second.find(name);
      if (column == composite->second.end()) {
        unknown_fields.insert(type + "." + name);
        continue;
      }
      validate(field, column->second, ddl, where + "." + name);
    }
    return;
  }
  if (type == "jsonb") return;
  if (type == "boolean") {
    require(value.type() == JsonType::Boolean, where, "expected boolean");
    return;
  }
  require(value.type() == JsonType::String, where, "expected string for " + type);
  const auto text = value.get_string().str();
  if (type == "bytea") {
    require(text.size() >= 4 && text.starts_with("\\x") && text.substr(2, 2) == "01" &&
                text.size() % 2 == 0 &&
                std::all_of(text.begin() + 2, text.end(), [](unsigned char c) { return std::isxdigit(c); }),
            where, "expected codec-1 hex bytea");
  } else if (type == "numeric" || type == "bigint" || type == "integer") {
    static const std::regex digits("^-?[0-9]+$");
    require(std::regex_match(text, digits), where, "expected decimal integer string");
  } else if (type == "tonaddr") {
    static const std::regex address("^-?[0-9]+:[0-9A-F]{64}$");
    require(std::regex_match(text, address), where, "expected wc:HEX address");
  } else if (type == "tonhash") {
    auto decoded = td::base64_decode(text);
    require(decoded.is_ok() && decoded.ok().size() == 32 && td::base64_encode(decoded.ok()) == text,
            where, "expected base64 hash");
  } else {
    require(type == "varchar" || type == "text" || type == "tonbytes", where, "unhandled DDL type " + type);
  }
}

td::JsonValue parse(std::string &json, const std::string &where) {
  auto result = td::json_decode(json);
  require(result.is_ok(), where, "invalid JSON");
  return result.move_as_ok();
}

void check_action(const Action &action, const ActionRowContext &ctx, const Ddl &ddl) {
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
  validate(accounts, "action_accounts[]", ddl, action.action_id + ".action_accounts");
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
    const auto ddl = read_ddl();
    std::ifstream input(MCH_ACTION_FIXTURES);
    require(input.good(), MCH_ACTION_FIXTURES, "cannot open fixture manifest");
    std::string manifest_json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    auto manifest = parse(manifest_json, MCH_ACTION_FIXTURES);
    const auto *fixtures = jfield(manifest, "fixtures");
    require(fixtures != nullptr && fixtures->type() == JsonType::Object &&
                fixtures->get_object().field_count() != 0, MCH_ACTION_FIXTURES, "empty fixture manifest");
    const auto &matchers = gen_matchers_ir();
    const auto setup = prepare_classify(matchers);
    require(!setup.table_missing && !setup.fn_missing, "classify", setup.error);
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
      for (const auto &action : actions) check_action(action, context, ddl);
      auto batch_json = action_rows_json(actions, context);
      auto batch = parse(batch_json, id.str());
      require(batch.type() == JsonType::Array && batch.get_array().size() == actions.size(), id.str(), "wrong row array");
      for (std::size_t i = 0; i < actions.size(); ++i) {
        const auto &row = batch.get_array()[i];
        require(jfield(row, "payload") != nullptr, actions[i].action_id, "missing payload");
        for (const auto &field : row.get_object().field_values_) {
          const auto name = field.first.str();
          require(name != "extra" && !name.ends_with("_data"), actions[i].action_id,
                  "legacy payload column " + name);
        }
        validate(row, "actions", ddl, actions[i].action_id);
      }
      row_count += actions.size();
    }
    require(row_count != 0, "fixtures", "no actions checked");
    std::string unknown;
    for (const auto &field : unknown_fields) unknown += " " + field;
    require(unknown.empty(), "ddl", "fields absent from the DDL:" + unknown);

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
    check_action(action, context, ddl);
    action.data = Value::make_dict({{"comment", Value::make_str(std::string("a\0b", 3))}});
    auto comment_json = action_legacy_details_json(action);
    require(comment_json.find("\\u0000") == std::string::npos,
            action.action_id + ".data.comment", "JSON contains escaped NUL");
    auto comment_row = parse(comment_json, action.action_id);
    require(jstr(comment_row, "comment") == "ab",
            action.action_id + ".data.comment", "NUL must be stripped");
    action.data = Value::make_dict({{"parent_gasless_action", Value::make_str(context.trace_id)}});
    auto extra_json = action_legacy_details_json(action);
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
