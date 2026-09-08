// SCAFFOLDING
#include "accelerator_codegen/stateful/core/aux_maintenance_decision_log.hpp"

#include <mutex>

namespace accelerator_codegen::aux_state::core {
namespace {

std::mutex &LogMutex() {
  static std::mutex mutex;
  return mutex;
}

MaintenanceDecisionLogEntry &LogSlot() {
  static MaintenanceDecisionLogEntry entry;
  return entry;
}

} // namespace

void RecordMaintenanceSelectorDecision(const MaintenanceDecisionLogEntry &entry) {
  std::lock_guard<std::mutex> guard(LogMutex());
  auto &slot = LogSlot();
  slot = entry;
  slot.valid = true;
}

void RecordStagedMaintenanceAction(maintenance::MaintenanceAction action) {
  std::lock_guard<std::mutex> guard(LogMutex());
  auto &slot = LogSlot();
  slot.staged_action = action;
  if (!slot.valid) {
    slot.selected = action;
    slot.valid = true;
  }
}

void RecordMaintenanceActualCostUs(double actual_us) {
  std::lock_guard<std::mutex> guard(LogMutex());
  auto &slot = LogSlot();
  slot.actual_maintenance_us = actual_us;
  slot.valid = true;
}

std::optional<MaintenanceDecisionLogEntry> LastMaintenanceDecision() {
  std::lock_guard<std::mutex> guard(LogMutex());
  const auto &slot = LogSlot();
  if (!slot.valid) {
    return std::nullopt;
  }
  return slot;
}

void ClearMaintenanceDecisionLog() {
  std::lock_guard<std::mutex> guard(LogMutex());
  LogSlot() = MaintenanceDecisionLogEntry{};
}

} // namespace accelerator_codegen::aux_state::core
