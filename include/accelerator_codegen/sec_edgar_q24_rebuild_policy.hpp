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

bool SecEdgarQ24RebuildMaintenanceEnabled();

uint64_t GetSecEdgarQ24PartitionRebuildCostUs();
uint64_t GetSecEdgarQ24FullRebuildCostUs();

bool HasSecEdgarQ24InterWriteMeasurement();
uint64_t GetSecEdgarQ24MeasuredInterWriteIntervalUs();

void RecordSecEdgarQ24RebuildPolicyPreCommit();
void ResetSecEdgarQ24RebuildPolicyTiming();

bool PrepareSecEdgarQ24RebuildPreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer);

void EnsurePendingSecEdgarQ24RebuildApplied(duckdb::ClientContext &context);

void ApplySecEdgarQ24RebuildPlan(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                        const aux_state::InvalidationPlan &plan);

} // namespace accelerator_codegen
