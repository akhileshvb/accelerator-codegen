#pragma once

#include "accelerator_codegen/aux_state/invalidation_plan.hpp"
#include "accelerator_codegen/aux_state/partitioned_state_store.hpp"

namespace duckdb {
class ClientContext;
}

namespace accelerator_codegen {

namespace aux_state {
class TxnInvalidationBuffer;
}

bool Q10RebuildMaintenanceEnabled();

uint64_t GetQ10PartitionRebuildCostUs();
uint64_t GetQ10FullRebuildCostUs();

bool HasQ10InterWriteMeasurement();
uint64_t GetQ10MeasuredInterWriteIntervalUs();

void RecordQ10RebuildPolicyPreCommit();
void ResetQ10RebuildPolicyTiming();

bool PrepareQ10RebuildPreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer);

void EnsurePendingQ10RebuildApplied(duckdb::ClientContext &context);

void ApplyQ10RebuildPlan(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                         const aux_state::InvalidationPlan &plan);

} // namespace accelerator_codegen
