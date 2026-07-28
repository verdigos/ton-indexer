#include "EmuTraceConvert.h"

#include "DataParser.h"  // ParseQuery::parse_transaction (EM1 seam)

#include "vm/boc.h"

namespace mch {

td::Result<std::vector<schema::Transaction>> emu_to_schema_txs(const EmuTraceSnapshot &snap,
                                                               int global_version) {
  std::vector<schema::Transaction> txs;
  txs.reserve(snap.nodes.size());
  for (std::size_t i = 0; i < snap.nodes.size(); i++) {
    const EmuTxRec &node = snap.nodes[i];
    if (node.tx_boc.empty()) {
      return td::Status::Error(PSLICE() << "node " << i << " has no transaction cell");
    }
    TRY_RESULT_PREFIX(tx_root, vm::std_boc_deserialize(node.tx_boc.as_slice()),
                      PSLICE() << "node " << i << ": ");
    auto r_tx = ParseQuery::parse_transaction(tx_root, node.address.workchain, global_version);
    if (r_tx.is_error()) {
      return r_tx.move_as_error_prefix(PSLICE() << "node " << i << ": ");
    }
    schema::Transaction tx = r_tx.move_as_ok();
    tx.mc_seqno = node.mc_seqno;
    txs.push_back(std::move(tx));
  }
  return txs;
}

}  // namespace mch
