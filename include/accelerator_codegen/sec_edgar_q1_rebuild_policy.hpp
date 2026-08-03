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

bool SecEdgarQ1RebuildMaintenanceEnabled();

uint64_t GetSecEdgarQ1PartitionRebuildCostUs();
uint64_t GetSecEdgarQ1FullRebuildCostUs();

bool HasSecEdgarQ1InterWriteMeasurement();
uint64_t GetSecEdgarQ1MeasuredInterWriteIntervalUs();

void RecordSecEdgarQ1RebuildPolicyPreCommit();
void ResetSecEdgarQ1RebuildPolicyTiming();

bool PrepareSecEdgarQ1RebuildPreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer);

void EnsurePendingSecEdgarQ1RebuildApplied(duckdb::ClientContext &context);

void ApplySecEdgarQ1RebuildPlan(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                        const aux_state::InvalidationPlan &plan);

} // namespace accelerator_codegen
