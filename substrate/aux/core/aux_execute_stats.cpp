// SCAFFOLDING
// Execute-path counters and per-thread store resolve time.
#include "substrate/aux/core/aux_execute_stats.hpp"

#include <atomic>
#include <chrono>

namespace accelerator_codegen::aux_state::core {
namespace {
std::atomic<uint64_t> g_hit{0}, g_rebuilt{0}, g_fallback{0};
thread_local double t_resolve_us = 0;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
} // namespace

void AddStoreResolveMicros(double us) { t_resolve_us += us; }
double ThreadStoreResolveMicros() { return t_resolve_us; }
StoreResolveTimer::StoreResolveTimer() : start_ns_(NowNs()) {}
StoreResolveTimer::~StoreResolveTimer() { t_resolve_us += (NowNs() - start_ns_) / 1000.0; }

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
