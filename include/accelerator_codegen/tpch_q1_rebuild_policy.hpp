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

bool Q1RebuildMaintenanceEnabled();

uint64_t GetQ1PartitionRebuildCostUs();
uint64_t GetQ1FullRebuildCostUs();

bool HasQ1InterWriteMeasurement();
uint64_t GetQ1MeasuredInterWriteIntervalUs();

void RecordQ1RebuildPolicyPreCommit();
void ResetQ1RebuildPolicyTiming();

bool PrepareQ1RebuildPreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer);

void EnsurePendingQ1RebuildApplied(duckdb::ClientContext &context);

void ApplyQ1RebuildPlan(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                        const aux_state::InvalidationPlan &plan);

} // namespace accelerator_codegen
