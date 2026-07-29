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

bool Q12RebuildMaintenanceEnabled();

uint64_t GetQ12PartitionRebuildCostUs();
uint64_t GetQ12FullRebuildCostUs();

bool HasQ12InterWriteMeasurement();
uint64_t GetQ12MeasuredInterWriteIntervalUs();

void RecordQ12RebuildPolicyPreCommit();
void ResetQ12RebuildPolicyTiming();

bool PrepareQ12RebuildPreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                               const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer);

void EnsurePendingQ12RebuildApplied(duckdb::ClientContext &context);

void ApplyQ12RebuildPlan(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                         const aux_state::InvalidationPlan &plan);

} // namespace accelerator_codegen
