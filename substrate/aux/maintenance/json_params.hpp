// SCAFFOLDING
// Minimal flat-JSON number lookup for cost-model files.
#pragma once

#include <cstdint>
#include <string>

namespace accelerator_codegen::aux_state::maintenance {

inline double ParseJsonDouble(const std::string &content, const std::string &key, double fallback) {
  const auto pos = content.find('"' + key + '"');
  if (pos == std::string::npos) {
    return fallback;
  }
  const auto colon = content.find(':', pos);
  if (colon == std::string::npos) {
    return fallback;
  }
  return std::stod(content.substr(colon + 1));
}

inline uint64_t ParseJsonUint64(const std::string &content, const std::string &key, uint64_t fallback) {
  return static_cast<uint64_t>(ParseJsonDouble(content, key, static_cast<double>(fallback)));
}

} // namespace accelerator_codegen::aux_state::maintenance
