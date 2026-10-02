// SCAFFOLDING
#include "accelerator_codegen/stateful/core/aux_execute_stats.hpp"

#include <atomic>

namespace accelerator_codegen::aux_state::core {
namespace {
std::atomic<uint64_t> g_hit{0}, g_rebuilt{0}, g_fallback{0};
} // namespace

void RecordAuxExecute(AuxExecutePath path) {
  switch (path) {
  case AuxExecutePath::Hit:
    g_hit++;
    break;
  case AuxExecutePath::RebuiltOnRead:
    g_rebuilt++;
    break;
  case AuxExecutePath::Fallback:
    g_fallback++;
    break;
  }
}

AuxExecuteCounts ReadAuxExecuteCounts() {
  return {g_hit.load(), g_rebuilt.load(), g_fallback.load()};
}

void ResetAuxExecuteCounts() {
  g_hit = 0;
  g_rebuilt = 0;
  g_fallback = 0;
}

std::string FormatAuxExecuteCounts(const AuxExecuteCounts &counts) {
  return "hit=" + std::to_string(counts.hit) +
         " rebuilt_on_read=" + std::to_string(counts.rebuilt_on_read) +
         " fallback=" + std::to_string(counts.fallback);
}

} // namespace accelerator_codegen::aux_state::core
