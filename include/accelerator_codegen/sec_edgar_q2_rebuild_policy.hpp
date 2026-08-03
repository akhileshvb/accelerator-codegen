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

bool SecEdgarQ2RebuildMaintenanceEnabled();

uint64_t GetSecEdgarQ2PartitionRebuildCostUs();
uint64_t GetSecEdgarQ2FullRebuildCostUs();

bool HasSecEdgarQ2InterWriteMeasurement();
uint64_t GetSecEdgarQ2MeasuredInterWriteIntervalUs();

void RecordSecEdgarQ2RebuildPolicyPreCommit();
void ResetSecEdgarQ2RebuildPolicyTiming();

bool PrepareSecEdgarQ2RebuildPreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer);

void EnsurePendingSecEdgarQ2RebuildApplied(duckdb::ClientContext &context);

void ApplySecEdgarQ2RebuildPlan(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                        const aux_state::InvalidationPlan &plan);

} // namespace accelerator_codegen
