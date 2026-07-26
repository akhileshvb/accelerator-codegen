#pragma once

#include <string>
#include <unordered_map>
#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"
#include "accelerator_codegen/aux_state/partition_key.hpp"
#include "accelerator_codegen/tpch_q14_incremental.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/main/client_context_state.hpp"

namespace accelerator_codegen::aux_state {

class AcceleratorStateRegistry;

class TxnInvalidationBuffer final : public duckdb::MutationConsumer {
public:
	static constexpr const char *STATE_KEY = "accel_txn_invalidation";

	void SetRegistry(AcceleratorStateRegistry *registry_p) {
		registry = registry_p;
	}

	void RecordPlan(const InvalidationPlan &plan);
	bool IsPartitionBypassed(PartitionKey key) const;
	bool IsAllBypassed() const;
	void SetPendingQ14Rebuild(const InvalidationPlan &plan);
	bool HasPendingQ14Rebuild() const;
	const InvalidationPlan &PendingQ14Rebuild() const;
	void ClearPendingQ14Rebuild();
	Q14IncrementalTxnBuffer &GetQ14Incremental() {
		return q14_incremental;
	}
	const InvalidationPlan &CommittedPendingApply() const {
		return committed_pending_apply;
	}
	const InvalidationPlan &TxnLocalBypass() const {
		return txn_local_bypass;
	}
	maintenance::StagedMaintenanceOutcome &GetOrCreateStagedOutcome(const std::string &accelerator_id);
	maintenance::StagedMaintenanceOutcome &GetStagedOutcome(const std::string &accelerator_id);

	void OnTableMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event) override;
	void TransactionPreCommit(duckdb::MetaTransaction &transaction, duckdb::ClientContext &context) override;
	void TransactionCommit(duckdb::MetaTransaction &transaction, duckdb::ClientContext &context) override;
	void TransactionRollback(duckdb::MetaTransaction &transaction, duckdb::ClientContext &context) override;

private:
	AcceleratorStateRegistry *registry = nullptr;
	InvalidationPlan committed_pending_apply;
	InvalidationPlan txn_local_bypass;
	InvalidationPlan q14_pending_rebuild;
	bool q14_pending_rebuild_active = false;
	Q14IncrementalTxnBuffer q14_incremental;
	std::unordered_map<std::string, maintenance::StagedMaintenanceOutcome> staged_outcomes;
};

duckdb::shared_ptr<TxnInvalidationBuffer> GetOrCreateTxnInvalidationBuffer(duckdb::ClientContext &context);

} // namespace accelerator_codegen::aux_state
