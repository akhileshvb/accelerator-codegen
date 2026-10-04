// SCAFFOLDING
#include "accelerator_codegen/stateful/core/aux_store_diagnostics.hpp"

#include <algorithm>
#include <cstdlib>

namespace accelerator_codegen::aux_state::core {
namespace {

double Median(std::vector<double> samples) {
  if (samples.empty()) {
    return 0.0;
  }
  std::sort(samples.begin(), samples.end());
  const auto middle = samples.size() / 2;
  if (samples.size() % 2 == 0) {
    return (samples[middle - 1] + samples[middle]) / 2.0;
  }
  return samples[middle];
}

// Samples are bounded: once full, the oldest are overwritten (a ring), so a
// long-running reader thread does not grow without bound.
constexpr std::size_t kMaxSamples = 1 << 14;

void PushBounded(std::vector<double> &samples, std::size_t &next, double value) {
  if (samples.size() < kMaxSamples) {
    samples.push_back(value);
    return;
  }
  samples[next % kMaxSamples] = value;
  ++next;
}

} // namespace

AuxStoreDiagnosticsCollector &AuxStoreDiagnosticsCollector::ThreadLocal() {
  thread_local AuxStoreDiagnosticsCollector collector;
  return collector;
}

void AuxStoreDiagnosticsCollector::Reset() {
  current_execute_partitions_ = 0;
  current_execute_bytes_ = 0;
  validity_slots_scanned_samples_.clear();
  validity_scan_us_samples_.clear();
  partitions_read_samples_.clear();
  state_bytes_read_samples_.clear();
  execute_calls_ = 0;
  validity_next_ = validity_us_next_ = partitions_next_ = bytes_next_ = 0;
}

void AuxStoreDiagnosticsCollector::RecordValidityScan(uint64_t slots_scanned,
                                                      double scan_us) {
  PushBounded(validity_slots_scanned_samples_, validity_next_, static_cast<double>(slots_scanned));
  PushBounded(validity_scan_us_samples_, validity_us_next_, scan_us);
}

void AuxStoreDiagnosticsCollector::RecordPartitionsRead(uint64_t partitions,
                                                        uint64_t bytes_read) {
  current_execute_partitions_ += partitions;
  current_execute_bytes_ += bytes_read;
}

void AuxStoreDiagnosticsCollector::BeginExecute() {
  current_execute_partitions_ = 0;
  current_execute_bytes_ = 0;
}

void AuxStoreDiagnosticsCollector::EndExecute() {
  PushBounded(partitions_read_samples_, partitions_next_,
              static_cast<double>(current_execute_partitions_));
  if (AuxStoreProfilingBytesEnabled()) {
    PushBounded(state_bytes_read_samples_, bytes_next_,
                static_cast<double>(current_execute_bytes_));
  }
  execute_calls_++;
  current_execute_partitions_ = 0;
  current_execute_bytes_ = 0;
}

AuxStoreDiagnosticsSnapshot AuxStoreDiagnosticsCollector::Snapshot(
    uint64_t valid_partitions, uint64_t total_slots) const {
  AuxStoreDiagnosticsSnapshot snapshot;
  snapshot.valid_partitions = valid_partitions;
  snapshot.total_slots = total_slots;
  snapshot.validity_slots_scanned_median =
      Median(validity_slots_scanned_samples_);
  snapshot.validity_scan_us_median = Median(validity_scan_us_samples_);
  snapshot.partitions_read_per_query_median = Median(partitions_read_samples_);
  snapshot.state_bytes_read_median = static_cast<uint64_t>(
      Median(state_bytes_read_samples_));
  snapshot.execute_calls = execute_calls_;
  return snapshot;
}

bool AuxStoreProfilingBytesEnabled() {
  static const bool enabled = [] {
    const char *value = std::getenv("ACCEL_AUX_PROFILE");
    return value && *value && value[0] != '0';
  }();
  return enabled;
}

bool AuxStoreDiagnosticsEnabled() {
  static const bool enabled = [] {
    const char *value = std::getenv("ACCEL_AUX_DIAG");
    return !(value && value[0] == '0');
  }();
  return enabled;
}

void RecordValidityScan(uint64_t slots_scanned, double scan_us) {
  AuxStoreDiagnosticsCollector::ThreadLocal().RecordValidityScan(slots_scanned,
                                                                 scan_us);
}

void RecordPartitionsRead(uint64_t partitions, uint64_t bytes_read) {
  AuxStoreDiagnosticsCollector::ThreadLocal().RecordPartitionsRead(partitions,
                                                                   bytes_read);
}

void BeginAuxExecuteProfiling() {
  if (!AuxStoreDiagnosticsEnabled()) return;
  AuxStoreDiagnosticsCollector::ThreadLocal().BeginExecute();
}

void EndAuxExecuteProfiling() {
  if (!AuxStoreDiagnosticsEnabled()) return;
  AuxStoreDiagnosticsCollector::ThreadLocal().EndExecute();
}

void ResetAuxStoreDiagnosticsProfiling() {
  AuxStoreDiagnosticsCollector::ThreadLocal().Reset();
}

} // namespace accelerator_codegen::aux_state::core
