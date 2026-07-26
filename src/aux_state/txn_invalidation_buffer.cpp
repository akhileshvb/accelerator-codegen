#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/invalidation_plan.hpp"
#include "accelerator_codegen/tpch_q14_aux.hpp"
#include "accelerator_codegen/tpch_q14_incremental.hpp"
#include "accelerator_codegen/tpch_q14_maintenance.hpp"
#include "accelerator_codegen/tpch_q14_write_policy.hpp"
#include "duckdb/common/shared_ptr.hpp"

namespace accelerator_codegen::aux_state {

void TxnInvalidationBuffer::RecordPlan(const InvalidationPlan &plan) {
	if (plan.IsEmpty()) {
		return;
	}
	MergeInvalidationPlan(committed_pending_apply, plan);
	MergeInvalidationPlan(txn_local_bypass, plan);
}

bool TxnInvalidationBuffer::IsPartitionBypassed(PartitionKey key) const {
	if (txn_local_bypass.scope == InvalidationScope::AllPartitions) {
		return true;
	}
	if (txn_local_bypass.scope != InvalidationScope::PartitionSet) {
		return false;
	}
	return txn_local_bypass.partitions.find(key) != txn_local_bypass.partitions.end();
}

bool TxnInvalidationBuffer::IsAllBypassed() const {
	return txn_local_bypass.scope == InvalidationScope::AllPartitions;
}

void TxnInvalidationBuffer::SetPendingQ14Rebuild(const InvalidationPlan &plan) {
	q14_pending_rebuild = plan;
	q14_pending_rebuild_active = true;
}

bool TxnInvalidationBuffer::HasPendingQ14Rebuild() const {
	return q14_pending_rebuild_active;
}

const InvalidationPlan &TxnInvalidationBuffer::PendingQ14Rebuild() const {
	return q14_pending_rebuild;
}

void TxnInvalidationBuffer::ClearPendingQ14Rebuild() {
	q14_pending_rebuild = InvalidationPlan {};
	q14_pending_rebuild_active = false;
}

maintenance::StagedMaintenanceOutcome &TxnInvalidationBuffer::GetOrCreateStagedOutcome(const std::string &accelerator_id) {
	return staged_outcomes[accelerator_id];
}

maintenance::StagedMaintenanceOutcome &TxnInvalidationBuffer::GetStagedOutcome(const std::string &accelerator_id) {
	return staged_outcomes[accelerator_id];
}

namespace {

const TableDependency *FindTableDependency(const std::vector<TableDependency> &dependencies,
                                           const std::string &table_name) {
	for (const auto &dependency : dependencies) {
		if (dependency.table_name == table_name) {
			return &dependency;
		}
	}
	return nullptr;
}

} // namespace

void TxnInvalidationBuffer::OnTableMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event) {
	if (!registry) {
		return;
	}
	const auto &q14_dependencies = registry->GetAcceleratorDependencies("tpch_q14_aux");
	const bool dependent = IsQ14DependentTableMutation(event, q14_dependencies);
	InvalidationPlan effective = registry->InterpretMutation(event);

	const auto *lineitem = FindTableDependency(q14_dependencies, "lineitem");
	const auto *part = FindTableDependency(q14_dependencies, "part");
	if (lineitem && event.table_oid == lineitem->table_oid) {
		Q14AccumulateLineitemMutationLight(context, event, q14_dependencies, q14_incremental);
	} else if (part && event.table_oid == part->table_oid) {
		Q14AccumulatePartMutationLight(context, event, q14_dependencies, q14_incremental);
	}
	MergeInvalidationPlan(effective, q14_incremental.ExtraInvalidationPlan());

	if (dependent) {
		RecordQ14DependentTableMutation(effective.IsEmpty());
		RecordQ14WorkloadRelevantWrite();
	}
	RecordPlan(effective);
}

void TxnInvalidationBuffer::TransactionPreCommit(duckdb::MetaTransaction &transaction, duckdb::ClientContext &context) {
	(void)transaction;
	if (!registry || committed_pending_apply.IsEmpty()) {
		if (!registry) {
			return;
		}
		if (!q14_incremental.HasPendingChanges()) {
			return;
		}
	}
	registry->ApplyPreCommitInvalidationAll(committed_pending_apply, &context, this);
}

void TxnInvalidationBuffer::TransactionCommit(duckdb::MetaTransaction &transaction, duckdb::ClientContext &context) {
	(void)transaction;
	if (registry) {
		registry->PublishMaintenanceOnCommit(context, *this);
	}
	committed_pending_apply = InvalidationPlan {};
	txn_local_bypass = InvalidationPlan {};
	q14_incremental.Clear();
	staged_outcomes.clear();
}

void TxnInvalidationBuffer::TransactionRollback(duckdb::MetaTransaction &transaction, duckdb::ClientContext &context) {
	(void)transaction;
	(void)context;
	if (registry) {
		registry->DiscardMaintenanceOnRollback(*this);
	}
	committed_pending_apply = InvalidationPlan {};
	txn_local_bypass = InvalidationPlan {};
	q14_incremental.Clear();
	staged_outcomes.clear();
	ClearPendingQ14Rebuild();
}

duckdb::shared_ptr<TxnInvalidationBuffer> GetOrCreateTxnInvalidationBuffer(duckdb::ClientContext &context) {
	return context.registered_state->GetOrCreate<TxnInvalidationBuffer>(TxnInvalidationBuffer::STATE_KEY);
}

} // namespace accelerator_codegen::aux_state
