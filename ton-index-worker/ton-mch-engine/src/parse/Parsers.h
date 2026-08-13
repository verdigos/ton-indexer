// Internal registry surface: the per-family message-body parse fns, one TU per
// protocol family (ParseJettons.cpp, ParseDns.cpp, ...). Declared here so
// MsgParse.cpp's message_parsers() map can reference them across TU boundaries.
// NOT part of the public MsgParse.h surface.
#pragma once

#include "../Value.h"

#include "td/utils/Status.h"
#include "vm/cells/Cell.h"

namespace mch {

// Only the kept-hand irregular / single-use parsers are declared below. The
// protocol-owned generated ABI parsers are registered by AbiBridge.cpp.

// Jetton family (ParseJettons.cpp): forward-payload-tail transfer.
td::Result<Value> parse_jetton_transfer(const td::Ref<vm::Cell> &body);

// DNS family (ParseDns.cpp).
td::Result<Value> parse_change_dns(const td::Ref<vm::Cell> &body);

// Vesting family (ParseVesting.cpp).
td::Result<Value> parse_vesting_send_message(const td::Ref<vm::Cell> &body);
td::Result<Value> parse_vesting_add_whitelist(const td::Ref<vm::Cell> &body);

}  // namespace mch
