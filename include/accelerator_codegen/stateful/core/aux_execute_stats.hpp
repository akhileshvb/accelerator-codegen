// SCAFFOLDING
#pragma once

#include <cstdint>
#include <string>

namespace accelerator_codegen::aux_state::core {

// How each accelerator query was answered (process-wide counters). Engine
// runtimes record one per Execute; tests read them to check that SQL
// fallback stays a last resort.
enum class AuxExecutePath : uint8_t {
  Hit,            // from the current state
  RebuiltOnRead,  // the state was (re)built by this query, then read
  Fallback,       // SQL from the tables (no usable state)
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

} // namespace accelerator_codegen::aux_state::core
