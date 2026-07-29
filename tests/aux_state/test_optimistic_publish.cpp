#include "accelerator_codegen/aux_state/maintenance/partition_publish.hpp"
#include "accelerator_codegen/aux_state/partitioned_state_store.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

using namespace accelerator_codegen::aux_state;
using namespace accelerator_codegen::aux_state::maintenance;

void TestStaleVersionInvalidates() {
	PartitionedStateStore store;
	store.PublishPartition(199506, std::make_shared<int>(1), 1);
	const auto captured_version = store.GetPartitionVersion(199506);
	store.PublishPartition(199506, std::make_shared<int>(3), 1);

	StagedPartitionUpdate stale_update;
	stale_update.key = 199506;
	stale_update.payload = std::make_shared<int>(2);
	stale_update.expected_partition_version = captured_version;

	PublishValidatedPartitionUpdates(store, {stale_update}, 2);
	Require(store.GetStatus(199506) == PartitionStatus::Invalid, "stale publish invalidates partition");
}

} // namespace

int main() {
	try {
		TestStaleVersionInvalidates();
		std::cout << "test_optimistic_publish passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_optimistic_publish failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
