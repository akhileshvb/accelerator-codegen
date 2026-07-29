#include "accelerator_codegen/aux_state/reader_mutation_epoch.hpp"

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

void TestPinEpochPerStatement() {
	duckdb::DuckDB database(":memory:");
	duckdb::Connection connection(database);
	const auto first = GetReaderVisibleMutationEpoch(*connection.context, "tpch_q14_aux", 5);
	const auto second = GetReaderVisibleMutationEpoch(*connection.context, "tpch_q14_aux", 9);
	Require(first == 5, "first pin uses statement-start epoch");
	Require(second == 5, "second call returns pinned epoch");
}

} // namespace

int main() {
	try {
		TestPinEpochPerStatement();
		std::cout << "test_reader_epoch_visibility passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_reader_epoch_visibility failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
