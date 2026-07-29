#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/execution_gate.hpp"
#include "accelerator_codegen/aux_state/invalidation_plan.hpp"
#include "accelerator_codegen/aux_state/maintenance/maintenance_action.hpp"
#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"
#include "accelerator_codegen/aux_state/partitioned_state_store.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

#include "duckdb.hpp"

namespace {

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

using namespace accelerator_codegen::aux_state;

void TestMergeInvalidationPlan() {
	InvalidationPlan accum;
	InvalidationPlan delta;
	delta.scope = InvalidationScope::PartitionSet;
	delta.partitions.insert(199501);
	MergeInvalidationPlan(accum, delta);
	Require(accum.scope == InvalidationScope::PartitionSet, "merge partition set");
	Require(accum.partitions.count(199501) == 1, "merged partition present");

	InvalidationPlan all;
	all.scope = InvalidationScope::AllPartitions;
	MergeInvalidationPlan(accum, all);
	Require(accum.scope == InvalidationScope::AllPartitions, "all partitions wins");
}

void TestSubtractInvalidationPlan() {
	InvalidationPlan pending;
	pending.scope = InvalidationScope::PartitionSet;
	pending.partitions.insert(199501);
	pending.partitions.insert(199502);

	InvalidationPlan completed;
	completed.scope = InvalidationScope::PartitionSet;
	completed.partitions.insert(199501);
	SubtractInvalidationPlan(pending, completed);
	Require(pending.partitions.count(199501) == 0, "completed partition removed");
	Require(pending.partitions.count(199502) == 1, "other partition remains");
}

void TestEmptyPartitionSetIsEmpty() {
	InvalidationPlan plan;
	plan.scope = InvalidationScope::PartitionSet;
	Require(plan.IsEmpty(), "empty partition set is empty");

	InvalidationPlan accum;
	MergeInvalidationPlan(accum, plan);
	Require(accum.IsEmpty(), "merging empty partition set leaves accumulator empty");
}

void TestPartitionedStateStore() {
	PartitionedStateStore store;
	Require(store.GetStatus(199501) == PartitionStatus::Absent, "absent initially");

	auto payload = std::make_shared<int>(42);
	store.PublishPartition(199501, payload, 1);
	Require(store.GetStatus(199501) == PartitionStatus::Valid, "valid after publish");
	Require(store.GetReadablePayload(199501) != nullptr, "payload readable");

	store.InvalidatePartition(199501);
	Require(store.GetStatus(199501) == PartitionStatus::Invalid, "invalid after invalidate");
	Require(store.GetReadablePayload(199501) == nullptr, "payload not readable when invalid");

	store.PublishPartition(199502, std::make_shared<int>(7), 2);
	store.PublishPartition(199503, std::make_shared<int>(8), 2);
	Require(store.ValidPartitionCount() == 2, "two valid partitions");
	store.InvalidateAll();
	Require(store.ValidPartitionCount() == 0, "none valid after invalidate all");
}

void TestTxnInvalidationBufferRyow() {
	TxnInvalidationBuffer buffer;
	InvalidationPlan plan;
	plan.scope = InvalidationScope::PartitionSet;
	plan.partitions.insert(199506);
	buffer.RecordPlan(plan);

	Require(buffer.IsPartitionBypassed(199506), "writer bypasses touched partition");
	Require(!buffer.IsPartitionBypassed(199507), "other partitions not bypassed");
	Require(!buffer.IsAllBypassed(), "not all bypassed");

	PartitionedStateStore store;
	store.PublishPartition(199506, std::make_shared<int>(1), 1);
	store.PublishPartition(199507, std::make_shared<int>(2), 1);

	auto bypassed = CheckExecutionGate(store, buffer, 199506, 1);
	Require(!bypassed.use_aux, "bypassed partition cannot use aux");
	Require(bypassed.bypassed_by_txn, "bypass flag set");

	auto other = CheckExecutionGate(store, buffer, 199507, 1);
	Require(other.use_aux, "untouched valid partition still accelerated");
}

void TestTxnInvalidationBufferGlobalBypass() {
	TxnInvalidationBuffer buffer;
	InvalidationPlan plan;
	plan.scope = InvalidationScope::AllPartitions;
	buffer.RecordPlan(plan);

	PartitionedStateStore store;
	store.PublishPartition(199501, std::make_shared<int>(1), 1);
	auto gate = CheckExecutionGate(store, buffer, 199501, 1);
	Require(!gate.use_aux, "global bypass blocks aux");
	Require(buffer.IsAllBypassed(), "all bypassed");
}

void TestTxnInvalidationBufferPreCommitApplies() {
	TxnInvalidationBuffer buffer;
	AcceleratorStateRegistry registry;
	registry.RegisterAccelerator("test", [](const duckdb::TableMutationEvent &,
	                                        const std::vector<TableDependency> &) { return InvalidationPlan {}; });
	buffer.SetRegistry(&registry);
	auto &store = registry.GetStore("test");
	store.PublishPartition(199501, std::make_shared<int>(1), 1);

	InvalidationPlan plan;
	plan.scope = InvalidationScope::PartitionSet;
	plan.partitions.insert(199501);
	buffer.RecordPlan(plan);
	Require(store.GetStatus(199501) == PartitionStatus::Valid, "valid before precommit");

	duckdb::DuckDB db(nullptr);
	duckdb::Connection con(db);
	duckdb::MetaTransaction txn(*con.context, duckdb::Timestamp::GetCurrentTimestamp(), 1);
	buffer.TransactionPreCommit(txn, *con.context);
	Require(store.GetStatus(199501) == PartitionStatus::Invalid, "invalidated at precommit");
	Require(!buffer.TxnLocalBypass().IsEmpty(), "bypass still active until commit");

	buffer.TransactionCommit(txn, *con.context);
	Require(buffer.TxnLocalBypass().IsEmpty(), "bypass cleared on commit");
}

void TestTxnInvalidationBufferRollback() {
	TxnInvalidationBuffer buffer;
	InvalidationPlan plan;
	plan.scope = InvalidationScope::PartitionSet;
	plan.partitions.insert(199501);
	buffer.RecordPlan(plan);

	duckdb::DuckDB db(nullptr);
	duckdb::Connection con(db);
	duckdb::MetaTransaction txn(*con.context, duckdb::Timestamp::GetCurrentTimestamp(), 1);
	buffer.TransactionRollback(txn, *con.context);
	Require(buffer.TxnLocalBypass().IsEmpty(), "bypass cleared on rollback");
	Require(buffer.CommittedPendingApply().IsEmpty(), "pending cleared on rollback");
}

void TestStagedMaintenanceOutcomeRollbackDiscards() {
	using namespace accelerator_codegen::aux_state::maintenance;

	TxnInvalidationBuffer buffer;
	auto &outcome = buffer.GetOrCreateStagedOutcome("test_accel");
	outcome.active = true;
	outcome.selected = MaintenanceAction::ApplyDelta;
	outcome.partition_updates.push_back(
	    StagedPartitionUpdate {199501, std::make_shared<int>(99), 1});
	outcome.invalidations.scope = InvalidationScope::PartitionSet;
	outcome.invalidations.partitions.insert(199502);

	PartitionedStateStore store;
	store.PublishPartition(199501, std::make_shared<int>(1), 1);
	store.PublishPartition(199502, std::make_shared<int>(2), 1);
	Require(store.GetStatus(199501) == PartitionStatus::Valid, "partition valid before rollback");
	Require(store.GetStatus(199502) == PartitionStatus::Valid, "second partition valid before rollback");

	duckdb::DuckDB db(nullptr);
	duckdb::Connection con(db);
	duckdb::MetaTransaction txn(*con.context, duckdb::Timestamp::GetCurrentTimestamp(), 1);
	buffer.TransactionRollback(txn, *con.context);

	Require(store.GetStatus(199501) == PartitionStatus::Valid, "rollback must not publish staged partition update");
	Require(store.GetStatus(199502) == PartitionStatus::Valid, "rollback must not apply staged invalidation");
	Require(!buffer.GetStagedOutcome("test_accel").active, "staged outcome cleared on rollback");
}

} // namespace

int main() {
	try {
		TestMergeInvalidationPlan();
		TestSubtractInvalidationPlan();
		TestEmptyPartitionSetIsEmpty();
		TestPartitionedStateStore();
		TestTxnInvalidationBufferRyow();
		TestTxnInvalidationBufferGlobalBypass();
		TestTxnInvalidationBufferPreCommitApplies();
		TestTxnInvalidationBufferRollback();
		TestStagedMaintenanceOutcomeRollbackDiscards();
		std::cout << "test_txn_invalidation passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_txn_invalidation failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
