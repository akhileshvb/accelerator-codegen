#include "accelerator_codegen/aux_state/execution_load.hpp"

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

void TestEnterExitSnapshot() {
	auto &registry = ExecutionLoadRegistryForTests();
	const auto token = registry.Enter(ExecutionKind::AuxQuery, "tpch_q14_aux", 100.0);
	auto snapshot = registry.Snapshot();
	Require(snapshot.active_aux_queries == 1, "one aux query active");
	Require(snapshot.total_active == 1, "total active is one");
	registry.Exit(token);
	snapshot = registry.Snapshot();
	Require(snapshot.total_active == 0, "no active after exit");
}

void TestScopeRaii() {
	{
		ActiveExecutionScope scope(ExecutionKind::LazyRebuild, "tpch_q14_aux");
		auto snapshot = GetExecutionLoadRegistry().Snapshot();
		Require(snapshot.active_rebuilds == 1, "rebuild scope active");
	}
	auto snapshot = GetExecutionLoadRegistry().Snapshot();
	Require(snapshot.active_rebuilds == 0, "scope released on destruction");
}

} // namespace

int main() {
	try {
		TestEnterExitSnapshot();
		TestScopeRaii();
		std::cout << "test_execution_load passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_execution_load failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
