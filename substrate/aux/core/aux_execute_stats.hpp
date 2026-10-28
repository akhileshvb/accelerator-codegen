// SCAFFOLDING
// Process-wide counters of how aux queries were answered, and per-thread store resolve time.
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace accelerator_codegen::aux_state::core {

// How each accelerator query was answered (process-wide counters). Engine
// runtimes record one per Execute; tests read them to check that SQL
// fallback stays a last resort.
enum class AuxExecutePath : uint8_t {
  Hit,           // from the current state
  Fallback,      // SQL from the tables (no usable state)
};

void RecordAuxExecute(AuxExecutePath path);

struct AuxExecuteCounts {
  uint64_t hit = 0;
  uint64_t rebuilt_on_read = 0; // never recorded; kept for the "hit= rebuilt_on_read= fallback=" format
  uint64_t fallback = 0;
};
AuxExecuteCounts ReadAuxExecuteCounts();
void ResetAuxExecuteCounts();
// Why queries fell back (a short cause name per reason site; recorded only on fallbacks).
void RecordAuxFallbackCause(const std::string &cause);
std::map<std::string, uint64_t> ReadAuxFallbackCauses();
// "hit=H rebuilt_on_read=R fallback=F"
std::string FormatAuxExecuteCounts(const AuxExecuteCounts &counts);

// Time a reader spends in an aux store choosing the state its snapshot sees
// (the store's lock, waiting for a pending commit, selecting the version):
// cumulative per thread. Stores add to it; a benchmark reads the difference
// around one query.
double ThreadStoreResolveMicros();

// Adds the time from construction to destruction to this thread's resolve time.
class StoreResolveTimer {
public:
  StoreResolveTimer();
  ~StoreResolveTimer();
  StoreResolveTimer(const StoreResolveTimer &) = delete;
  StoreResolveTimer &operator=(const StoreResolveTimer &) = delete;

private:
  int64_t start_ns_;
};

} // namespace accelerator_codegen::aux_state::core
