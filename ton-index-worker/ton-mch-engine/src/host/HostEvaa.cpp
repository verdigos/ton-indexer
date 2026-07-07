// EVAA host predicates (builders/evaa.py). See host/HostImpls.h for the
// internal registry surface and HostRegistry.h for the public one.
#include "host/HostImpls.h"

#include "host/HostCommon.h"

#include "BlockTree.h"
#include "MsgParse.h"

#include "vm/cellslice.h"

namespace mch {

namespace {

// _opcode_after_user_header: skip the EVAA user header (version coins + maybe
// upgrade-info ref + 2 exec bits), then compare the 32-bit opcode. Any
// malformed body never matches (Python's blanket except).
bool opcode_after_user_header(const Block *b, td::uint32 opcode) {
  if (b->btype != "call_contract") {  // isinstance(block, CallContractBlock)
    return false;
  }
  auto r_body = block_body(b);
  if (r_body.is_error()) {
    return false;
  }
  vm::CellSlice cs;
  bool special = false;
  try {
    cs = vm::load_cell_slice_special(r_body.ok(), special);
  } catch (...) {
    return false;
  }
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
  if (!cs.have(2 + 32)) {  // upgrade_exec + opcode
    return false;
  }
  cs.advance(2);
  return static_cast<td::uint32>(cs.fetch_ulong(32)) == opcode;
}

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

}  // namespace mch
