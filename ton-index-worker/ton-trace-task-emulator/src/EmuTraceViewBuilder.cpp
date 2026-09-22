#include "EmuTraceViewBuilder.h"
#include "emu/EmuClassifierBridge.h"

td::Result<mch::EmuTraceView> build_emu_trace_view(const Trace& trace) {
  mch::EmuTraceView view;
  view.trace_id = td::base64_encode(trace.ext_in_msg_hash_norm.as_slice());
  view.tx_limit_exceeded = trace.tx_limit_exceeded;
  view.interfaces = std::make_shared<const mch::TieredLookupSource::InterfaceMap>(mch::make_interface_map(trace));
  view.shard_states = trace.shard_states;
  view.config = trace.config;

  std::vector<const TraceNode*> pending{trace.root.get()};
  while (!pending.empty()) {
    const TraceNode* node = pending.back();
    pending.pop_back();
    if (!node) {
      continue;
    }
    TRY_RESULT(boc, vm::std_boc_serialize(node->transaction_root));
    mch::EmuTxRef ref;
    ref.address = node->address;
    ref.tx_boc = std::make_shared<const std::string>(boc.as_slice().str());
    ref.mc_seqno = node->mc_block_seqno;
    ref.finality = static_cast<mch::EmuFinality>(static_cast<std::uint8_t>(node->finality_state));
    view.nodes.push_back(std::move(ref));
    for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) {
      pending.push_back(it->get());
    }
  }
  return view;
}
