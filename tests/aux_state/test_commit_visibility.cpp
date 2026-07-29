#include "accelerator_codegen/aux_state/accelerator_maintenance_state.hpp"
#include "accelerator_codegen/aux_state/execution_gate.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/tpch_q10_aux.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

using namespace accelerator_codegen::aux_state;

void TestPerAcceleratorEpochIsolation() {
	PartitionedStateStore q14_store;
	PartitionedStateStore q10_store;
	AcceleratorMaintenanceState q14_maintenance;
	AcceleratorMaintenanceState q10_maintenance;

	const auto q14_epoch = q14_maintenance.BumpCommittedMutationEpoch();
	q14_store.PublishPartition(199506, std::make_shared<int>(42), q14_epoch);

	TxnInvalidationBuffer buffer;
	const auto gate_q14 = CheckExecutionGate(q14_store, buffer, 199506, q14_epoch);
	Require(gate_q14.use_aux, "q14 partition valid at its epoch");

	const auto gate_q10 = CheckExecutionGate(q10_store, buffer, 199506, q10_maintenance.GetCommittedMutationEpoch());
	Require(!gate_q10.use_aux, "q10 store unaffected by q14 epoch");
}

void TestInvalidPartitionBlocksAux() {
	PartitionedStateStore store;
	AcceleratorMaintenanceState maintenance;
	const auto published = maintenance.BumpCommittedMutationEpoch();
	store.PublishPartition(199506, std::make_shared<int>(1), published);
	store.InvalidatePartition(199506);

	TxnInvalidationBuffer buffer;
	const auto gate = CheckExecutionGate(store, buffer, 199506, published + 1);
	Require(!gate.use_aux, "invalid partition blocks aux");
	Require(gate.committed_invalid, "committed_invalid flag set");
}

void TestQ10WindowGateIgnoresUnaffectedPartitionEpochs() {
	PartitionedStateStore store;
	AcceleratorMaintenanceState maintenance;
	const auto epoch1 = maintenance.BumpCommittedMutationEpoch();
	store.PublishPartition(199306, std::make_shared<int>(1), epoch1);
	store.PublishPartition(199307, std::make_shared<int>(2), epoch1);

	const auto epoch2 = maintenance.BumpCommittedMutationEpoch();
	store.PublishPartition(199306, std::make_shared<int>(3), epoch2);

	TxnInvalidationBuffer buffer;
	const auto start = duckdb::Date::FromString("1993-06-01");
	const auto end = accelerator_codegen::Q10ComputeExclusiveEndDate(start);
	const auto gate = accelerator_codegen::CheckQ10WindowExecutionGate(store, buffer, start, end, epoch2);
	Require(gate.use_aux, "unaffected window partitions must not force fallback");
}

} // namespace

int main() {
	try {
		TestPerAcceleratorEpochIsolation();
		TestInvalidPartitionBlocksAux();
		TestQ10WindowGateIgnoresUnaffectedPartitionEpochs();
		std::cout << "test_commit_visibility passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_commit_visibility failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
