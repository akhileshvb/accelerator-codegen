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

bool SecEdgarQ6RebuildMaintenanceEnabled();

uint64_t GetSecEdgarQ6PartitionRebuildCostUs();
uint64_t GetSecEdgarQ6FullRebuildCostUs();

bool HasSecEdgarQ6InterWriteMeasurement();
uint64_t GetSecEdgarQ6MeasuredInterWriteIntervalUs();

void RecordSecEdgarQ6RebuildPolicyPreCommit();
void ResetSecEdgarQ6RebuildPolicyTiming();

bool PrepareSecEdgarQ6RebuildPreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer);

void EnsurePendingSecEdgarQ6RebuildApplied(duckdb::ClientContext &context);

void ApplySecEdgarQ6RebuildPlan(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                        const aux_state::InvalidationPlan &plan);

} // namespace accelerator_codegen
