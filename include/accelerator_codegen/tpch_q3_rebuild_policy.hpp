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

bool Q3RebuildMaintenanceEnabled();

uint64_t GetQ3PartitionRebuildCostUs();
uint64_t GetQ3FullRebuildCostUs();

bool HasQ3InterWriteMeasurement();
uint64_t GetQ3MeasuredInterWriteIntervalUs();

void RecordQ3RebuildPolicyPreCommit();
void ResetQ3RebuildPolicyTiming();

bool PrepareQ3RebuildPreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer);

void EnsurePendingQ3RebuildApplied(duckdb::ClientContext &context);

void ApplyQ3RebuildPlan(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                         const aux_state::InvalidationPlan &plan);

} // namespace accelerator_codegen
