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

bool SecEdgarQ3RebuildMaintenanceEnabled();

uint64_t GetSecEdgarQ3PartitionRebuildCostUs();
uint64_t GetSecEdgarQ3FullRebuildCostUs();

bool HasSecEdgarQ3InterWriteMeasurement();
uint64_t GetSecEdgarQ3MeasuredInterWriteIntervalUs();

void RecordSecEdgarQ3RebuildPolicyPreCommit();
void ResetSecEdgarQ3RebuildPolicyTiming();

bool PrepareSecEdgarQ3RebuildPreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer);

void EnsurePendingSecEdgarQ3RebuildApplied(duckdb::ClientContext &context);

void ApplySecEdgarQ3RebuildPlan(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                        const aux_state::InvalidationPlan &plan);

} // namespace accelerator_codegen
