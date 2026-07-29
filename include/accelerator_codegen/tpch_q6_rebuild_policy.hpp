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

bool Q6RebuildMaintenanceEnabled();

uint64_t GetQ6PartitionRebuildCostUs();
uint64_t GetQ6FullRebuildCostUs();

bool HasQ6InterWriteMeasurement();
uint64_t GetQ6MeasuredInterWriteIntervalUs();

void RecordQ6RebuildPolicyPreCommit();
void ResetQ6RebuildPolicyTiming();

bool PrepareQ6RebuildPreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer);

void EnsurePendingQ6RebuildApplied(duckdb::ClientContext &context);

void ApplyQ6RebuildPlan(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                         const aux_state::InvalidationPlan &plan);

} // namespace accelerator_codegen
