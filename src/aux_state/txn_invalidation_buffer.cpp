#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"

#include <stdexcept>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/invalidation_plan.hpp"
#include "duckdb/common/shared_ptr.hpp"

namespace accelerator_codegen::aux_state {

void TxnInvalidationBuffer::RecordPlan(const InvalidationPlan &plan) {
	if (plan.IsEmpty()) {
		return;
	}
	MergeInvalidationPlan(committed_pending_apply, plan);
	MergeInvalidationPlan(txn_local_bypass, plan);
}

void TxnInvalidationBuffer::RecordAcceleratorPlan(const std::string &accelerator_id, const InvalidationPlan &plan) {
	if (plan.IsEmpty()) {
		return;
	}
	MergeInvalidationPlan(accelerator_plans[accelerator_id], plan);
}

const InvalidationPlan &TxnInvalidationBuffer::AcceleratorPlan(const std::string &accelerator_id) const {
	static const InvalidationPlan kEmpty {};
	const auto it = accelerator_plans.find(accelerator_id);
	if (it == accelerator_plans.end()) {
		return kEmpty;
	}
	return it->second;
}

void TxnInvalidationBuffer::ClearAcceleratorPlans() {
	accelerator_plans.clear();
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

void TxnInvalidationBuffer::SetPendingLazyRebuild(const std::string &accelerator_id, const InvalidationPlan &plan) {
	pending_lazy_rebuilds[accelerator_id] = plan;
}

bool TxnInvalidationBuffer::HasPendingLazyRebuild(const std::string &accelerator_id) const {
	const auto it = pending_lazy_rebuilds.find(accelerator_id);
	return it != pending_lazy_rebuilds.end() && !it->second.IsEmpty();
}

const InvalidationPlan &TxnInvalidationBuffer::PendingLazyRebuild(const std::string &accelerator_id) const {
	static const InvalidationPlan kEmpty {};
	const auto it = pending_lazy_rebuilds.find(accelerator_id);
	if (it == pending_lazy_rebuilds.end()) {
		return kEmpty;
	}
	return it->second;
}

void TxnInvalidationBuffer::ClearPendingLazyRebuild(const std::string &accelerator_id) {
	pending_lazy_rebuilds.erase(accelerator_id);
}

void TxnInvalidationBuffer::ClearAllPendingLazyRebuilds() {
	pending_lazy_rebuilds.clear();
}

IncrementalTxnBuffer &TxnInvalidationBuffer::GetOrCreateIncrementalBuffer(const std::string &accelerator_id) {
	auto it = incremental_buffers.find(accelerator_id);
	if (it != incremental_buffers.end()) {
		return *it->second;
	}
	if (!registry) {
		throw std::runtime_error("txn invalidation buffer has no registry");
	}
	auto factory = registry->GetIncrementalBufferFactory(accelerator_id);
	if (!factory) {
		throw std::runtime_error("no incremental buffer factory for accelerator: " + accelerator_id);
	}
	auto created = factory();
	if (!created) {
		throw std::runtime_error("incremental buffer factory returned null for accelerator: " + accelerator_id);
	}
	auto &ref = *created;
	incremental_buffers.emplace(accelerator_id, std::move(created));
	return ref;
}

IncrementalTxnBuffer *TxnInvalidationBuffer::TryGetIncrementalBuffer(const std::string &accelerator_id) {
	const auto it = incremental_buffers.find(accelerator_id);
	if (it == incremental_buffers.end()) {
		return nullptr;
	}
	return it->second.get();
}

bool TxnInvalidationBuffer::AnyIncrementalHasPendingChanges() const {
	for (const auto &entry : incremental_buffers) {
		if (entry.second && entry.second->HasPendingChanges()) {
			return true;
		}
	}
	return false;
}

maintenance::StagedMaintenanceOutcome &TxnInvalidationBuffer::GetOrCreateStagedOutcome(const std::string &accelerator_id) {
	return staged_outcomes[accelerator_id];
}

maintenance::StagedMaintenanceOutcome &TxnInvalidationBuffer::GetStagedOutcome(const std::string &accelerator_id) {
	return staged_outcomes[accelerator_id];
}

void TxnInvalidationBuffer::OnTableMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event) {
	if (!registry) {
		return;
	}
	registry->OnTableMutation(context, event, *this);
}

void TxnInvalidationBuffer::TransactionPreCommit(duckdb::MetaTransaction &transaction, duckdb::ClientContext &context) {
	(void)transaction;
	if (!registry) {
		return;
	}
	if (committed_pending_apply.IsEmpty() && !AnyIncrementalHasPendingChanges()) {
		return;
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
	ClearAcceleratorPlans();
	for (auto &entry : incremental_buffers) {
		if (entry.second) {
			entry.second->Clear();
		}
	}
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
	ClearAcceleratorPlans();
	for (auto &entry : incremental_buffers) {
		if (entry.second) {
			entry.second->Clear();
		}
	}
	staged_outcomes.clear();
	ClearAllPendingLazyRebuilds();
}

duckdb::shared_ptr<TxnInvalidationBuffer> GetOrCreateTxnInvalidationBuffer(duckdb::ClientContext &context) {
	return context.registered_state->GetOrCreate<TxnInvalidationBuffer>(TxnInvalidationBuffer::STATE_KEY);
}

} // namespace accelerator_codegen::aux_state
