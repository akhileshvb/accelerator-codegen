// Conformance: substrate Store + MaintenancePolicy + Coordinator vs real C++.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_maintenance_policy.hpp"
#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/maintenance/cost_model.hpp"
#include "accelerator_codegen/stateful/maintenance/maintenance_action.hpp"
#include "accelerator_codegen/stateful/state/partitioned_state_store.hpp"

using accelerator_codegen::aux_state::PartitionKey;
using accelerator_codegen::aux_state::PartitionStatus;
using accelerator_codegen::aux_state::PartitionedStateStore;
using accelerator_codegen::aux_state::core::MaintenanceDecision;
using accelerator_codegen::aux_state::core::MaintenanceRequest;
using accelerator_codegen::aux_state::core::SelectAuxMaintenanceAction;
using accelerator_codegen::aux_state::core::SetForcedMaintenanceActionName;
using accelerator_codegen::aux_state::maintenance::CostModelParams;
using accelerator_codegen::aux_state::maintenance::MaintenanceAction;

namespace {

void CheckPublishThenReadable() {
  PartitionedStateStore store;
  const PartitionKey key = 42;
  auto payload = std::make_shared<int>(7);
  store.PublishPartition(key, payload, /*epoch=*/10);
  assert(store.GetStatus(key) == PartitionStatus::Valid);
  assert(store.GetReadablePayload(key) == payload);
  assert(store.GetPartitionVersion(key) == 1);
  assert(store.GetValidThroughMutationEpoch(key) == 10);
}

void CheckTryPublishCasFail() {
  PartitionedStateStore store;
  const PartitionKey key = 1;
  store.PublishPartition(key, std::make_shared<int>(1), 0);
  const auto v = store.GetPartitionVersion(key);
  assert(v == 1);
  // Wrong expected version → Invalid, no readable payload
  bool ok = store.TryPublishPartition(key, std::make_shared<int>(99), 0, /*expected=*/99);
  assert(!ok);
  assert(store.GetStatus(key) == PartitionStatus::Invalid);
  assert(store.GetReadablePayload(key) == nullptr);
}

void CheckTryPublishSuccess() {
  PartitionedStateStore store;
  const PartitionKey key = 5;
  auto payload = std::make_shared<int>(3);
  assert(store.TryPublishPartition(key, payload, 1, /*expected=*/0));
  assert(store.GetStatus(key) == PartitionStatus::Valid);
  assert(store.GetReadablePayload(key) == payload);
}

void CheckInvalidate() {
  PartitionedStateStore store;
  const PartitionKey key = 9;
  store.PublishPartition(key, std::make_shared<int>(1), 0);
  store.InvalidatePartition(key);
  assert(store.GetStatus(key) == PartitionStatus::Invalid);
  assert(store.GetReadablePayload(key) == nullptr);
}

void CheckMaintenanceForcedAndNoOp() {
  CostModelParams params{};
  SetForcedMaintenanceActionName("ApplyDelta");
  MaintenanceRequest req{};
  req.rows_touched = 100;
  req.incremental_supported = true;
  req.full_rebuild_supported = true;
  auto d = SelectAuxMaintenanceAction(req, params);
  assert(d.action == MaintenanceAction::ApplyDelta);
  SetForcedMaintenanceActionName(""); // clear

  MaintenanceRequest zero{};
  zero.rows_touched = 0;
  zero.whole_state_required = false;
  d = SelectAuxMaintenanceAction(zero, params);
  assert(d.action == MaintenanceAction::NoOp);
}

void CheckMaintenanceApplyDeltaEligible() {
  SetForcedMaintenanceActionName("");
  CostModelParams params{};
  params.apply_delta_max_rows = 1000;
  params.apply_delta_overhead_us = 1;
  params.apply_delta_base_us = 1;
  params.apply_delta_per_row_us = 1;
  params.full_rebuild_us = 1e9;
  params.invalidation_overhead_us = 1e9;
  params.partition_rebuild_us = 1e9;
  params.fallback_us = 100;
  params.aux_hit_us = 10;

  MaintenanceRequest req{};
  req.rows_touched = 10;
  req.affected_partition_count = 1;
  req.incremental_supported = true;
  req.partition_rebuild_supported = true;
  req.full_rebuild_supported = true;
  req.expected_queries_before_next_write = 100;
  auto d = SelectAuxMaintenanceAction(req, params);
  assert(d.action == MaintenanceAction::ApplyDelta);
}

} // namespace

int main() {
  CheckPublishThenReadable();
  CheckTryPublishCasFail();
  CheckTryPublishSuccess();
  CheckInvalidate();
  CheckMaintenanceForcedAndNoOp();
  CheckMaintenanceApplyDeltaEligible();
  return 0;
}
