#pragma once

#include "ActionBuild.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace schema { struct Trace; }

namespace mch {

struct ActionRowContext {
  std::string trace_id;
  std::optional<std::string> trace_external_hash, trace_external_hash_norm;
  std::int64_t trace_end_lt;
  std::int32_t trace_end_utime;
  std::int32_t trace_mc_seqno_end;
};

ActionRowContext row_context(const schema::Trace &trace);
std::string action_row_json(const Action &a, const ActionRowContext &ctx);
std::string action_account_rows_json(const Action &a, const ActionRowContext &ctx);
std::string action_rows_json(const std::vector<Action> &rows, const ActionRowContext &ctx);

}  // namespace mch
