#include "accelerator_codegen/aux_state/accelerator_maintenance_state.hpp"
#include "accelerator_codegen/aux_state/invalidation_plan.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

using namespace accelerator_codegen::aux_state;

void TestClaimedSnapshotSubtract() {
	AcceleratorMaintenanceState maintenance;
	InvalidationPlan plan_a;
	plan_a.scope = InvalidationScope::PartitionSet;
	plan_a.partitions.insert(199506);
	maintenance.MergeGlobalPendingLazyRebuild(plan_a);

	auto claim = maintenance.BeginRebuild();
	Require(claim.HasWork(), "claim should have work");
	Require(claim.captured_plan.partitions.count(199506) == 1, "claim includes A");

	InvalidationPlan plan_b;
	plan_b.scope = InvalidationScope::PartitionSet;
	plan_b.partitions.insert(199507);
	maintenance.MergeGlobalPendingLazyRebuild(plan_b);

	InvalidationPlan completed;
	completed.scope = InvalidationScope::PartitionSet;
	completed.partitions.insert(199506);
	maintenance.CompleteRebuild(claim, completed);

	auto follow_up = maintenance.BeginRebuild();
	Require(follow_up.HasWork(), "B should remain pending");
	Require(follow_up.captured_plan.partitions.count(199506) == 0, "A removed from pending");
	Require(follow_up.captured_plan.partitions.count(199507) == 1, "B still pending");
}

} // namespace

int main() {
	try {
		TestClaimedSnapshotSubtract();
		std::cout << "test_epoch_lazy_rebuild passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_epoch_lazy_rebuild failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
