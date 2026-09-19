#include "smc-envelope/SmartContract.h"
#include "td/utils/logging.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cells/MerkleProof.h"
#include "vm/cellslice.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/wait.h>

namespace {

td::Ref<vm::Cell> cell_from_bytes(const unsigned char *bytes, std::size_t n) {
  vm::CellBuilder cb;
  cb.store_bytes(bytes, n);
  return cb.finalize();
}

// run_get_method pushes method_id (Int) on TOS; c4 is state.data.
// LDREF / LDU leave the leftover slice on TOS (value underneath).
// 0x30 DROP          drop method_id
// 0xed44 PUSH c4     data cell
// 0xd0 CTOS
// 0xd4 LDREF         ref under leftover slice
// 0x30 DROP          drop leftover, keep ref
// 0xd0 CTOS          payload slice
// 0xd307 LDU 8       uint under leftover slice
// 0x30 DROP          keep the uint
const unsigned char kLoadCode[] = {0x30, 0xed, 0x44, 0xd0, 0xd4, 0x30, 0xd0, 0xd3, 0x07, 0x30};

// Same through the data ref, then unwrap a Merkle proof and CTOS a pruned child.
// 0x30 DROP / 0xed44 PUSH c4 / 0xd0 CTOS / 0xd4 LDREF / 0x30 DROP
// 0xd739 XCTOS / 0x30 DROP (is_special flag)
// 0xd4 LDREF / 0x30 DROP / 0xd0 CTOS  (virtualized root)
// 0xd4 LDREF / 0x30 DROP / 0xd0 CTOS  (pruned child)
const unsigned char kProofCode[] = {0x30, 0xed, 0x44, 0xd0, 0xd4, 0x30, 0xd7, 0x39, 0x30,
                                    0xd4, 0x30, 0xd0, 0xd4, 0x30, 0xd0};

// wrap(virtualized root): CTOS the root, then CTOS its first (pruned) child.
// 0x30 DROP / 0xed44 PUSH c4 / 0xd0 CTOS / 0xd4 LDREF / 0x30 DROP
// 0xd0 CTOS / 0xd4 LDREF / 0x30 DROP / 0xd0 CTOS
const unsigned char kVirtCode[] = {0x30, 0xed, 0x44, 0xd0, 0xd4, 0x30, 0xd0, 0xd4, 0x30, 0xd0};

td::Ref<vm::Cell> wrap_data(td::Ref<vm::Cell> inner) {
  return vm::CellBuilder().store_ref(std::move(inner)).finalize();
}

td::Ref<vm::Cell> ordinary_payload() {
  return vm::CellBuilder().store_long(42, 8).finalize();
}

td::Ref<vm::Cell> pruned_payload() {
  auto child = vm::CellBuilder().store_long(1, 8).finalize();
  auto inner = vm::CellBuilder().store_long(42, 8).store_ref(std::move(child)).finalize();
  return vm::CellBuilder::create_pruned_branch(std::move(inner), 1);
}

// proof -> ref0 (inner) -> ref0 must be a PrunedBranch: that is the cell
// kProofCode / kVirtCode CTOS after the last LDREF.
void dump_cell(const char *label, const td::Ref<vm::Cell> &c) {
  if (c.is_null()) {
    std::fprintf(stderr, "%s null\n", label);
    return;
  }
  vm::CellSlice cs(vm::NoVm(), c);
  std::fprintf(stderr, "%s special_type=%d level=%u\n", label, static_cast<int>(cs.special_type()),
               static_cast<unsigned>(c->get_level()));
}

void require_loaded_child_pruned(const td::Ref<vm::Cell> &proof) {
  td::Ref<vm::Cell> inner;
  td::Ref<vm::Cell> child;
  if (proof.not_null()) {
    vm::CellSlice ps(vm::NoVm(), proof);
    if (ps.have_refs(1)) {
      inner = ps.prefetch_ref(0);
    }
  }
  if (inner.not_null()) {
    vm::CellSlice is(vm::NoVm(), inner);
    if (is.have_refs(1)) {
      child = is.prefetch_ref(0);
    }
  }
  bool ok = child.not_null();
  if (ok) {
    vm::CellSlice cs(vm::NoVm(), child);
    ok = cs.is_special() && cs.special_type() == vm::Cell::SpecialType::PrunnedBranch;
  }
  if (!ok) {
    dump_cell("proof", proof);
    dump_cell("proof->ref0", inner);
    dump_cell("proof->ref0->ref0", child);
    std::_Exit(5);
  }
}

td::Ref<vm::Cell> merkle_proof_payload() {
  auto payload = ordinary_payload();
  // create_pruned_branch returns the original leaf when it has no refs; the
  // branch constructor always emits a PrunedBranch.
  auto pruned = vm::CellBuilder::do_create_pruned_branch(payload, 1);
  auto inner = vm::CellBuilder().store_ref(pruned).finalize();
  auto proof = vm::CellBuilder::create_merkle_proof(inner);
  require_loaded_child_pruned(proof);
  return proof;
}

td::Ref<vm::Cell> missing_library_payload() {
  unsigned char hash[32];
  std::memset(hash, 0xaa, sizeof(hash));
  vm::CellBuilder cb;
  cb.store_long(static_cast<td::uint8>(vm::Cell::SpecialType::Library), 8);
  cb.store_bytes(hash, sizeof(hash));
  return cb.finalize(true);
}

int run_variant(const char *name) {
  td::Ref<vm::Cell> code;
  td::Ref<vm::Cell> data;
  if (std::strcmp(name, "control") == 0) {
    code = cell_from_bytes(kLoadCode, sizeof(kLoadCode));
    data = wrap_data(ordinary_payload());
  } else if (std::strcmp(name, "pruned_data") == 0) {
    code = cell_from_bytes(kLoadCode, sizeof(kLoadCode));
    data = wrap_data(pruned_payload());
  } else if (std::strcmp(name, "merkle_proof_data") == 0) {
    code = cell_from_bytes(kProofCode, sizeof(kProofCode));
    data = wrap_data(merkle_proof_payload());
  } else if (std::strcmp(name, "virtualized_pruned") == 0) {
    auto proof = merkle_proof_payload();
    auto virt = vm::MerkleProof::virtualize(proof);
    if (virt.is_error()) {
      std::_Exit(6);
    }
    code = cell_from_bytes(kVirtCode, sizeof(kVirtCode));
    data = wrap_data(virt.move_as_ok());
  } else if (std::strcmp(name, "library_cell_missing") == 0) {
    code = cell_from_bytes(kLoadCode, sizeof(kLoadCode));
    data = wrap_data(missing_library_payload());
  } else {
    std::_Exit(4);
  }
  ton::SmartContract smc({std::move(code), std::move(data)});
  auto answer = smc.run_get_method(ton::SmartContract::Args().set_method_id(0));
  if (std::strcmp(name, "control") == 0) {
    // vm_code 0 with an empty/wrong stack would mean we never CTOS'd the ref.
    if (answer.code != 0) {
      return answer.code;
    }
    if (answer.stack.is_null() || answer.stack->depth() < 1) {
      return 99;
    }
    auto v = answer.stack->tos().as_int();
    if (v.is_null() || v->to_long() != 42) {
      return 99;
    }
    return 0;
  }
  return answer.code;
}

struct Expect {
  const char *name;
  const char *outcome;  // "exit:0" or "signal:N"
  const char *vm_code;  // integer or "-"
};

// Predictions from VmState::run() catch list: VmError/VmNoGas become exit codes;
// VmVirtError is not caught and SmartContract's catch (...) LOG(FATAL) abort()s.
const Expect kExpect[] = {
    {"control", "exit:0", "0"},
    {"pruned_data", "exit:0", "9"},
    {"merkle_proof_data", "exit:0", "9"},
    {"library_cell_missing", "exit:0", "9"},
    {"virtualized_pruned", "signal:6", "-"},
};

bool run_forked(const Expect &expect, std::string &got_outcome, std::string &got_vm) {
  int fds[2];
  if (pipe(fds) != 0) {
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    close(fds[0]);
    int code = run_variant(expect.name);
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%d", code);
    if (n > 0) {
      (void)write(fds[1], buf, static_cast<std::size_t>(n));
    }
    close(fds[1]);
    std::_Exit(0);
  }
  close(fds[1]);
  char buf[32] = {};
  ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
  close(fds[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return false;
  }
  if (WIFSIGNALED(status)) {
    got_outcome = "signal:" + std::to_string(WTERMSIG(status));
    got_vm = "-";
  } else {
    got_outcome = "exit:" + std::to_string(WEXITSTATUS(status));
    got_vm = n > 0 ? std::string(buf) : std::string("-");
  }
  return true;
}

}  // namespace

int main() {
  td::set_verbosity_level(0);
  int failures = 0;
  for (const auto &expect : kExpect) {
    std::string outcome;
    std::string vm_code;
    if (!run_forked(expect, outcome, vm_code)) {
      std::printf("variant=%s outcome=exit:1 vm_code=-\n", expect.name);
      ++failures;
      continue;
    }
    std::printf("variant=%s outcome=%s vm_code=%s\n", expect.name, outcome.c_str(), vm_code.c_str());
    if (outcome != expect.outcome || vm_code != expect.vm_code) {
      ++failures;
    }
  }
  return failures ? 1 : 0;
}
