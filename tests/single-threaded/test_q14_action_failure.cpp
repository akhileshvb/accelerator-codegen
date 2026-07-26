#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q14_aux.hpp"
#include "accelerator_codegen/tpch_q14_write_policy.hpp"
#include "test_support/q14_aux_helpers.hpp"

#include <iostream>
#include <stdexcept>

#include "duckdb.hpp"

namespace {

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

void CreateQ14Schema(duckdb::Connection &connection) {
	connection.Query(
	    "CREATE TABLE part(p_partkey BIGINT, p_name VARCHAR, p_mfgr VARCHAR, p_brand VARCHAR, p_type VARCHAR, "
	    "p_size INTEGER, p_container VARCHAR, p_retailprice DECIMAL(15,2), p_comment VARCHAR);"
	    "CREATE TABLE lineitem(l_orderkey BIGINT, l_partkey BIGINT, l_suppkey BIGINT, l_linenumber BIGINT, "
	    "l_quantity DECIMAL(15,2), l_extendedprice DECIMAL(15,2), l_discount DECIMAL(15,2), l_tax DECIMAL(15,2), "
	    "l_returnflag VARCHAR, l_linestatus VARCHAR, l_shipdate DATE, l_commitdate DATE, l_receiptdate DATE, "
	    "l_shipinstruct VARCHAR, l_shipmode VARCHAR, l_comment VARCHAR)");
}

void SeedBaseline(duckdb::Connection &connection) {
	connection.Query("INSERT INTO part VALUES (1, 'n', 'm', 'b', 'PROMO STEEL', 1, 'SM', 10.00, 'c')");
	connection.Query("INSERT INTO part VALUES (2, 'n', 'm', 'b', 'LARGE STEEL', 1, 'SM', 10.00, 'c')");
	connection.Query(
	    "INSERT INTO lineitem VALUES (1, 1, 1, 1, 1.00, 100.00, 0.00, 0.00, 'N', 'O', DATE '1995-01-15', "
	    "DATE '1995-01-15', DATE '1995-01-15', 'NONE', 'AIR', 'c')");
	connection.Query(
	    "INSERT INTO lineitem VALUES (2, 2, 1, 1, 1.00, 50.00, 0.00, 0.00, 'N', 'O', DATE '1995-01-20', "
	    "DATE '1995-01-20', DATE '1995-01-20', 'NONE', 'AIR', 'c')");
}

void BuildAux(duckdb::Connection &connection) {
	connection.Query("SELECT * FROM accel_q14_aux_build()");
}

void CheckQuery(duckdb::Connection &connection, const std::string &label, const std::string &month) {
	const auto sql = accelerator_codegen::test_support::AcceleratorQ14AuxSql(month);
	auto result = connection.Query(sql);
	if (!result || result->HasError()) {
		throw std::runtime_error(label + ": query failed: " + (result ? result->GetError() : "null result"));
	}
}

void TestHybridSafeFallbackAfterPoisonedInsert() {
	accelerator_codegen::test_support::EnableHybridMaintenance();

	duckdb::DuckDB database(nullptr);
	duckdb::Connection connection(database);
	CreateQ14Schema(connection);
	SeedBaseline(connection);
	accelerator_codegen::RegisterAcceleratorFunctions(connection);
	BuildAux(connection);

	connection.BeginTransaction();
	connection.Query(
	    "INSERT INTO lineitem VALUES (100, 99998, 1, 100, 1.00, 12.00, 0.00, 0.00, 'N', 'O', DATE '1995-01-22', "
	    "DATE '1995-01-22', DATE '1995-01-22', 'NONE', 'AIR', 'c')");
	connection.Commit();

	accelerator_codegen::test_support::RequireAllPartitionsInvalid(connection, "hybrid_poisoned_insert", "1995-01-01");
	CheckQuery(connection, "hybrid_poisoned_insert_query", "1995-01-01");
	Require(accelerator_codegen::GetQ14AuxDiagnostics().used_fallback,
	        "hybrid poisoned insert should answer via fallback");
	Require(accelerator_codegen::GetQ14AuxCounters().hybrid_decisions >= 1,
	        "hybrid must record a maintenance decision for poisoned insert");
}

} // namespace

int main() {
	try {
		TestHybridSafeFallbackAfterPoisonedInsert();
		std::cout << "test_q14_action_failure passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_q14_action_failure failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
