#pragma once

#include <memory>

#include "accelerator_codegen/aux_state/invalidation_plan.hpp"
#include "accelerator_codegen/aux_state/maintenance/maintenance_hook.hpp"
#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"

namespace duckdb {
class ClientContext;
}

namespace accelerator_codegen {

class SecEdgarQ24IncrementalTxnBuffer;

namespace aux_state {
class PartitionedStateStore;
class TxnInvalidationBuffer;
} // namespace aux_state

void ExecuteSecEdgarQ24MaintenancePreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                                   const aux_state::InvalidationPlan &plan, aux_state::TxnInvalidationBuffer &txn_buffer,
                                   aux_state::maintenance::StagedMaintenanceOutcome &outcome);

void PublishSecEdgarQ24MaintenanceOnCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                                  aux_state::TxnInvalidationBuffer &txn_buffer,
                                  aux_state::maintenance::StagedMaintenanceOutcome &outcome);

void DiscardSecEdgarQ24Maintenance(aux_state::maintenance::StagedMaintenanceOutcome &outcome);

std::shared_ptr<aux_state::maintenance::MaintenanceHook> CreateSecEdgarQ24MaintenanceHook();

} // namespace accelerator_codegen
