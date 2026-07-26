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

bool Q14RebuildMaintenanceEnabled();

uint64_t GetQ14PartitionRebuildCostUs();
uint64_t GetQ14FullRebuildCostUs();

bool HasQ14InterWriteMeasurement();
uint64_t GetQ14MeasuredInterWriteIntervalUs();

void RecordQ14RebuildPolicyPreCommit();
void ResetQ14RebuildPolicyTiming();

//! Cost-model gate at pre-commit. Returns true only for empty plans (no invalidation needed).
bool PrepareQ14RebuildPreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                                const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer);

void EnsurePendingQ14RebuildApplied(duckdb::ClientContext &context);

void ApplyQ14RebuildPlan(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                         const aux_state::InvalidationPlan &plan);

} // namespace accelerator_codegen
