#include "Surface.h"

#include "BuildRuntime.h"
#include "HostRegistry.h"
#include "MsgParse.h"
#include "Value.h"

#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace mch {

namespace {

// Minimal JSON string escape (registry names are plain identifiers today, but
// escape defensively so a stray control byte can never corrupt the emitted JSON).
std::string json_escape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// std::set / the keys of std::map iterate in sorted order, so `names` is already
// stable. Emit a one-line JSON array.
std::string json_array(const std::vector<std::string> &names) {
  std::string out = "[";
  for (size_t i = 0; i < names.size(); i++) {
    if (i != 0) out += ", ";
    out += '"';
    out += json_escape(names[i]);
    out += '"';
  }
  out += ']';
  return out;
}

template <typename Map>
std::vector<std::string> keys(const Map &m) {
  std::vector<std::string> out;
  out.reserve(m.size());
  for (const auto &kv : m) out.push_back(kv.first);
  return out;
}

std::vector<std::string> to_vec(const std::set<std::string> &s) {
  return std::vector<std::string>(s.begin(), s.end());
}

}  // namespace

int run_surface() {
  // Top-level keys emitted in sorted order; each array is sorted by construction
  // (std::set / std::map ordering). Two-space indent, deterministic.
  std::cout << "{\n";
  std::cout << "  \"boc_fields\": " << json_array(to_vec(boc_field_names())) << ",\n";
  std::cout << "  \"fns\": " << json_array(keys(host_fns())) << ",\n";
  std::cout << "  \"lookup_kinds\": " << json_array(to_vec(lookup_kinds())) << ",\n";
  std::cout << "  \"parsers\": " << json_array(keys(message_parsers())) << ",\n";
  std::cout << "  \"preds\": " << json_array(keys(host_predicates())) << ",\n";
  std::cout << "  \"shapers\": " << json_array(keys(host_shapers())) << "\n";
  std::cout << "}\n";
  return 0;
}

}  // namespace mch
