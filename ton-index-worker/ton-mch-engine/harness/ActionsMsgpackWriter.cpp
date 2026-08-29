#include "ActionsMsgpackWriter.h"

#include "EmuActionSerializeTest.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace mch {

int write_action_msgpack_fixture(const std::string &path) {
  const std::string bytes = mch::wire_fixture_bytes();
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::fprintf(stderr, "cannot open %s for writing\n", path.c_str());
    return 1;
  }
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  out.close();
  if (!out) {
    std::fprintf(stderr, "write to %s failed\n", path.c_str());
    return 1;
  }
  std::printf("wrote %zu bytes to %s\n", bytes.size(), path.c_str());
  return 0;
}

}  // namespace mch
