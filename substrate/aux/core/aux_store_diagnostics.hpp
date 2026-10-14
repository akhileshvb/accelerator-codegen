// SCAFFOLDING
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace accelerator_codegen::aux_state::core {

struct AuxStoreDiagnosticsSnapshot {
  uint64_t valid_partitions = 0;
  uint64_t total_slots = 0;
  double validity_slots_scanned_median = 0.0;
  double validity_scan_us_median = 0.0;
  double partitions_read_per_query_median = 0.0;
  uint64_t state_bytes_read_median = 0;
  uint64_t execute_calls = 0;
};

class AuxStoreDiagnosticsCollector {
public:
  static AuxStoreDiagnosticsCollector &ThreadLocal();

  void Reset();
  void RecordValidityScan(uint64_t slots_scanned, double scan_us);
  void RecordPartitionsRead(uint64_t partitions, uint64_t bytes_read = 0);

  void BeginExecute();
  void EndExecute();

  AuxStoreDiagnosticsSnapshot Snapshot(uint64_t valid_partitions,
                                     uint64_t total_slots) const;

private:
  uint64_t current_execute_partitions_ = 0;
  uint64_t current_execute_bytes_ = 0;
  std::vector<double> validity_slots_scanned_samples_;
  std::vector<double> validity_scan_us_samples_;
  std::vector<double> partitions_read_samples_;
  std::vector<double> state_bytes_read_samples_;
  uint64_t execute_calls_ = 0;
  std::size_t validity_next_ = 0, validity_us_next_ = 0, partitions_next_ = 0, bytes_next_ = 0;
};

bool AuxStoreProfilingBytesEnabled();
// Per-read diagnostics (validity-scan timing, partitions-read samples). On by
// default; ACCEL_AUX_DIAG=0 turns them off (concurrent/latency experiments).
bool AuxStoreDiagnosticsEnabled();

void RecordValidityScan(uint64_t slots_scanned, double scan_us);
void RecordPartitionsRead(uint64_t partitions, uint64_t bytes_read = 0);
void BeginAuxExecuteProfiling();
void EndAuxExecuteProfiling();
void ResetAuxStoreDiagnosticsProfiling();

} // namespace accelerator_codegen::aux_state::core
