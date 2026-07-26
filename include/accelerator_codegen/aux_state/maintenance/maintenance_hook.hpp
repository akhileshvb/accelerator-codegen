#pragma once

#include <memory>
#include <string>

#include "accelerator_codegen/aux_state/invalidation_plan.hpp"
#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"

namespace duckdb {
class ClientContext;
}

namespace accelerator_codegen::aux_state {

class PartitionedStateStore;
class TxnInvalidationBuffer;

namespace maintenance {

class MaintenanceHook {
public:
	virtual ~MaintenanceHook() = default;

	virtual void PlanMaintenance(duckdb::ClientContext &context, PartitionedStateStore &store,
	                             const InvalidationPlan &plan, TxnInvalidationBuffer &txn_buffer,
	                             StagedMaintenanceOutcome &outcome) = 0;

	virtual void PublishMaintenance(duckdb::ClientContext &context, PartitionedStateStore &store,
	                                TxnInvalidationBuffer &txn_buffer, StagedMaintenanceOutcome &outcome) = 0;

	virtual void DiscardMaintenance(StagedMaintenanceOutcome &outcome) = 0;
};

using MaintenanceHookPtr = std::shared_ptr<MaintenanceHook>;

} // namespace maintenance
} // namespace accelerator_codegen::aux_state
