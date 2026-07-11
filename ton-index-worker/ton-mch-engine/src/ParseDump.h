// --parse-dump CLI harness (split out of MsgParse per §11 item 2). MsgParse now
// holds only the TL-B registry + dispatch; this renders the fixture messages.
#pragma once

#include <string>
#include <vector>

namespace mch {

// `--parse-dump` mode: for every message in the fixture(s) whose opcode is a
// registered jetton-family opcode, parse the body and print a canonical line
// (byte-for-byte twin: scratchpad/py_msg_dump.py). Returns 0 on success.
int run_parse_dump(const std::vector<std::string> &paths);

}  // namespace mch
