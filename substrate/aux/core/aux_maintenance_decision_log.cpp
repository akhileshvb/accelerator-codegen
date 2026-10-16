// SCAFFOLDING
#include "substrate/aux/core/aux_maintenance_decision_log.hpp"

#include <cstdio>
#include <string>

#include <mutex>

namespace accelerator_codegen::aux_state::core {
namespace {

std::mutex &LogMutex() {
  static std::mutex mutex;
  return mutex;
}

thread_local MaintenanceDecisionLogEntry *t_thread_slot = nullptr;

MaintenanceDecisionLogEntry &LogSlot() {
  if (t_thread_slot)
    return *t_thread_slot;
  static MaintenanceDecisionLogEntry entry;
  return entry;
}

} // namespace

ScopedThreadMaintenanceDecisionLog::ScopedThreadMaintenanceDecisionLog() : previous_(t_thread_slot) {
  t_thread_slot = &slot_;
}

ScopedThreadMaintenanceDecisionLog::~ScopedThreadMaintenanceDecisionLog() { t_thread_slot = previous_; }

void SetGlobalMaintenanceDecision(const MaintenanceDecisionLogEntry &entry) {
  auto *saved = t_thread_slot;
  t_thread_slot = nullptr;
  {
    std::lock_guard<std::mutex> guard(LogMutex());
    LogSlot() = entry;
  }
  t_thread_slot = saved;
}

void RecordCoordinatorMaintenanceUs(double us) {
  std::lock_guard<std::mutex> guard(LogMutex());
  LogSlot().maintenance_us = us;
}

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

std::string FormatLastMaintenanceDecision() {
  const auto entry = LastMaintenanceDecision();
  if (!entry || !entry->valid) {
    return "";
  }
  return std::string("action=") + maintenance::MaintenanceActionName(entry->selected) +
         " rows=" + std::to_string(entry->rows_touched) +
         " partitions=" + std::to_string(entry->affected_partition_count) +
         " maintenance_us=" + std::to_string(entry->maintenance_us) +
         " data_rows=" + std::to_string(entry->data_rows_hint);
}

std::optional<MaintenanceDecisionLogEntry> ParseMaintenanceDecision(const std::string &text) {
  char action[64] = {0};
  unsigned long long rows = 0, partitions = 0, data_rows = 0;
  double maintenance = 0.0;
  if (std::sscanf(text.c_str(), "action=%63s rows=%llu partitions=%llu maintenance_us=%lf data_rows=%llu",
                  action, &rows, &partitions, &maintenance, &data_rows) < 3) {
    return std::nullopt;
  }
  MaintenanceDecisionLogEntry entry;
  entry.maintenance_us = maintenance;
  entry.valid = true;
  entry.rows_touched = rows;
  entry.affected_partition_count = partitions;
  entry.data_rows_hint = data_rows;
  for (const auto a : {maintenance::MaintenanceAction::NoOp, maintenance::MaintenanceAction::ApplyDelta,
                       maintenance::MaintenanceAction::RebuildPartition,
                       maintenance::MaintenanceAction::RebuildAll,
                       maintenance::MaintenanceAction::InvalidateAndRebuildOnRead}) {
    if (std::string(maintenance::MaintenanceActionName(a)) == action) {
      entry.selected = a;
    }
  }
  return entry;
}

} // namespace accelerator_codegen::aux_state::core
