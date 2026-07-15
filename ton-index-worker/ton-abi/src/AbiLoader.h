#pragma once

// AbiLoader: JSON -> AbiModel, fail-closed (T1.2). Rejects: unknown Ty kind,
// any nested ty_idx OOB, missing required field, abi_schema_version != "1.0",
// malformed bigint, duplicate declaration names, instantiation arity
// mismatch, intN/uintN/bitsN width limits, alias cycles, union prefix
// non-freedom (a variant's prefix bit-string strict-prefixing another's),
// prefix_len > 32. See docs/ton-abi-plan.md T1.2 row for the full list this
// mirrors.

#include "AbiModel.h"

#include "td/utils/Status.h"

#include <string>

namespace ton_abi {

// `json_text` is COPIED internally (td::json_decode parses in place); the
// caller doesn't need to keep the buffer alive.
td::Result<ContractABI> load_abi_from_json(const std::string &json_text);

}  // namespace ton_abi
