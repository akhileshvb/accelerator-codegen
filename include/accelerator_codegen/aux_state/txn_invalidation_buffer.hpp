#pragma once

#include <string>
#include <unordered_map>
#include "accelerator_codegen/aux_state/incremental_txn_buffer.hpp"
#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"
#include "accelerator_codegen/aux_state/partition_key.hpp"
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
	void RecordAcceleratorPlan(const std::string &accelerator_id, const InvalidationPlan &plan);
	const InvalidationPlan &AcceleratorPlan(const std::string &accelerator_id) const;
	void ClearAcceleratorPlans();
	bool IsPartitionBypassed(PartitionKey key) const;
	bool IsAllBypassed() const;

	void SetPendingLazyRebuild(const std::string &accelerator_id, const InvalidationPlan &plan);
	bool HasPendingLazyRebuild(const std::string &accelerator_id) const;
	const InvalidationPlan &PendingLazyRebuild(const std::string &accelerator_id) const;
	void ClearPendingLazyRebuild(const std::string &accelerator_id);
	void ClearAllPendingLazyRebuilds();

	IncrementalTxnBuffer &GetOrCreateIncrementalBuffer(const std::string &accelerator_id);
	IncrementalTxnBuffer *TryGetIncrementalBuffer(const std::string &accelerator_id);
	bool AnyIncrementalHasPendingChanges() const;

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
	std::unordered_map<std::string, InvalidationPlan> accelerator_plans;
	std::unordered_map<std::string, InvalidationPlan> pending_lazy_rebuilds;
	std::unordered_map<std::string, std::unique_ptr<IncrementalTxnBuffer>> incremental_buffers;
	std::unordered_map<std::string, maintenance::StagedMaintenanceOutcome> staged_outcomes;
};

duckdb::shared_ptr<TxnInvalidationBuffer> GetOrCreateTxnInvalidationBuffer(duckdb::ClientContext &context);

} // namespace accelerator_codegen::aux_state
