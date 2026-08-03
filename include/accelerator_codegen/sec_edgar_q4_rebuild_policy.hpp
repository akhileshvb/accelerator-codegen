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

bool SecEdgarQ4RebuildMaintenanceEnabled();

uint64_t GetSecEdgarQ4PartitionRebuildCostUs();
uint64_t GetSecEdgarQ4FullRebuildCostUs();

bool HasSecEdgarQ4InterWriteMeasurement();
uint64_t GetSecEdgarQ4MeasuredInterWriteIntervalUs();

void RecordSecEdgarQ4RebuildPolicyPreCommit();
void ResetSecEdgarQ4RebuildPolicyTiming();

bool PrepareSecEdgarQ4RebuildPreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer);

void EnsurePendingSecEdgarQ4RebuildApplied(duckdb::ClientContext &context);

void ApplySecEdgarQ4RebuildPlan(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                        const aux_state::InvalidationPlan &plan);

} // namespace accelerator_codegen
