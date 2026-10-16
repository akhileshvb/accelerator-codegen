// SCAFFOLDING
// Process-wide counters of how aux queries were answered, and per-thread store resolve time.
#pragma once

#include <cstdint>
#include <string>

namespace accelerator_codegen::aux_state::core {

// How each accelerator query was answered (process-wide counters). Engine
// runtimes record one per Execute; tests read them to check that SQL
// fallback stays a last resort.
enum class AuxExecutePath : uint8_t {
  Hit,           // from the current state
  RebuiltOnRead, // the state was (re)built by this query, then read
  Fallback,      // SQL from the tables (no usable state)
};

void RecordAuxExecute(AuxExecutePath path);

struct AuxExecuteCounts {
  uint64_t hit = 0;
  uint64_t rebuilt_on_read = 0;
  uint64_t fallback = 0;
};
AuxExecuteCounts ReadAuxExecuteCounts();
void ResetAuxExecuteCounts();
// "hit=H rebuilt_on_read=R fallback=F"
std::string FormatAuxExecuteCounts(const AuxExecuteCounts &counts);

// Time a reader spends in an aux store choosing the state its snapshot sees
// (the store's lock, waiting for a pending commit, selecting the version):
// cumulative per thread. Stores add to it; a benchmark reads the difference
// around one query.
void AddStoreResolveMicros(double us);
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
