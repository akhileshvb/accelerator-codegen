#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q14_aux.hpp"
#include "accelerator_codegen/tpch_q14_write_policy.hpp"
#include "test_support/q14_aux_helpers.hpp"

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

void CreateQ14Schema(duckdb::Connection &connection) {
	auto result = connection.Query(
	    "CREATE TABLE part("
	    "p_partkey BIGINT NOT NULL, p_name VARCHAR NOT NULL, p_mfgr VARCHAR NOT NULL, "
	    "p_brand VARCHAR NOT NULL, p_type VARCHAR, p_size INTEGER NOT NULL, "
	    "p_container VARCHAR NOT NULL, p_retailprice DECIMAL(15,2) NOT NULL, p_comment VARCHAR NOT NULL);"
	    "CREATE TABLE lineitem("
	    "l_orderkey BIGINT NOT NULL, l_partkey BIGINT NOT NULL, l_suppkey BIGINT NOT NULL, "
	    "l_linenumber BIGINT NOT NULL, l_quantity DECIMAL(15,2) NOT NULL, "
	    "l_extendedprice DECIMAL(15,2) NOT NULL, l_discount DECIMAL(15,2) NOT NULL, "
	    "l_tax DECIMAL(15,2) NOT NULL, l_returnflag VARCHAR NOT NULL, l_linestatus VARCHAR NOT NULL, "
	    "l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL, l_receiptdate DATE NOT NULL, "
	    "l_shipinstruct VARCHAR NOT NULL, l_shipmode VARCHAR NOT NULL, l_comment VARCHAR NOT NULL);");
	Require(result != nullptr && !result->HasError(), "failed to create q14 schema: " + result->GetError());
}

void SeedBaseline(duckdb::Connection &connection) {
	connection.Query("INSERT INTO part VALUES (1, 'n', 'm', 'b', 'PROMO STEEL', 1, 'SM', 10.00, 'c'), "
	                 "(2, 'n', 'm', 'b', 'LARGE STEEL', 1, 'SM', 10.00, 'c')");
	connection.Query("INSERT INTO lineitem VALUES "
	                 "(1, 1, 1, 1, 1.00, 100.00, 0.00, 0.00, 'N', 'O', DATE '1995-01-15', DATE '1995-01-15', "
	                 "DATE '1995-01-15', 'NONE', 'AIR', 'c'), "
	                 "(2, 2, 1, 1, 1.00, 200.00, 0.00, 0.00, 'N', 'O', DATE '1995-01-15', DATE '1995-01-15', "
	                 "DATE '1995-01-15', 'NONE', 'AIR', 'c')");
}

void BuildAux(duckdb::Connection &connection) {
	accelerator_codegen::ResetQ14AuxCounters();
	connection.Query("SELECT * FROM accel_q14_aux_build()");
}

void CheckQuery(duckdb::Connection &connection, const std::string &label, const std::string &month) {
	accelerator_codegen::test_support::RequireEqualQ14Results(
	    connection, label, accelerator_codegen::test_support::ReferenceQ14Sql(month),
	    accelerator_codegen::test_support::AcceleratorQ14AuxSql(month));
}

void TestHybridRelevantBehavior() {
	accelerator_codegen::test_support::EnableHybridMaintenance();
	duckdb::DuckDB database(nullptr);
	duckdb::Connection connection(database);
	CreateQ14Schema(connection);
	SeedBaseline(connection);
	accelerator_codegen::RegisterAcceleratorFunctions(connection);
	BuildAux(connection);
	accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(connection, "hybrid_baseline", "1995-01-01");

	const auto noops_before = accelerator_codegen::GetQ14AuxCounters().dependent_mutation_events_empty_plan;
	connection.BeginTransaction();
	connection.Query("UPDATE lineitem SET l_comment = 'x' WHERE l_orderkey = 1");
	connection.Commit();
	accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(connection, "hybrid_after_irrelevant", "1995-01-01");
	Require(accelerator_codegen::GetQ14AuxCounters().dependent_mutation_events_empty_plan > noops_before,
	        "hybrid counts irrelevant write noop");
	CheckQuery(connection, "hybrid_after_irrelevant_query", "1995-01-01");
	Require(accelerator_codegen::GetQ14AuxDiagnostics().used_aux_path,
	        "hybrid keeps aux after irrelevant write");

	connection.BeginTransaction();
	connection.Query("UPDATE lineitem SET l_extendedprice = 75.00 WHERE l_orderkey = 1");
	connection.Commit();
	accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(connection, "hybrid_after_relevant", "1995-01-01");
	CheckQuery(connection, "hybrid_after_relevant_query", "1995-01-01");
	Require(accelerator_codegen::GetQ14AuxCounters().action_selected_apply_delta >= 1,
	        "hybrid applies delta for point lineitem update");
}

void TestHybridIncrementalMaintenance() {
	accelerator_codegen::test_support::EnableHybridMaintenance();
	duckdb::DuckDB database(nullptr);
	duckdb::Connection connection(database);
	CreateQ14Schema(connection);
	SeedBaseline(connection);
	accelerator_codegen::RegisterAcceleratorFunctions(connection);
	BuildAux(connection);

	connection.BeginTransaction();
	connection.Query("INSERT INTO lineitem VALUES (3, 1, 1, 3, 1.00, 50.00, 0.00, 0.00, 'N', 'O', "
	                 "DATE '1995-01-20', DATE '1995-01-20', DATE '1995-01-20', 'NONE', 'AIR', 'c')");
	connection.Commit();
	accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(connection, "hybrid_after_insert", "1995-01-01");
	accelerator_codegen::test_support::RequireNoIncrementalFailures("hybrid_after_insert");
	CheckQuery(connection, "hybrid_after_insert_query", "1995-01-01");

	connection.BeginTransaction();
	connection.Query("UPDATE lineitem SET l_shipdate = DATE '1995-02-20' WHERE l_orderkey = 1");
	connection.Commit();
	accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(connection, "hybrid_after_move_src", "1995-01-01");
	accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(connection, "hybrid_after_move_dst", "1995-02-01");

	connection.BeginTransaction();
	connection.Query("UPDATE part SET p_type = 'PROMO STEEL' WHERE p_partkey = 2");
	connection.Commit();
	accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(connection, "hybrid_after_part_type_flip", "1995-01-01");
	CheckQuery(connection, "hybrid_after_part_type_flip_query", "1995-01-01");
}

void TestHybridIncrementalEdgeCases() {
	accelerator_codegen::test_support::EnableHybridMaintenance();
	duckdb::DuckDB database(nullptr);
	duckdb::Connection connection(database);
	CreateQ14Schema(connection);
	SeedBaseline(connection);
	accelerator_codegen::RegisterAcceleratorFunctions(connection);
	BuildAux(connection);

	connection.BeginTransaction();
	connection.Query("DELETE FROM lineitem WHERE l_orderkey = 2");
	connection.Commit();
	accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(connection, "hybrid_after_delete", "1995-01-01");
	CheckQuery(connection, "hybrid_after_delete_query", "1995-01-01");

	connection.BeginTransaction();
	connection.Query(
	    "INSERT INTO lineitem VALUES (99, 99999, 1, 99, 1.00, 10.00, 0.00, 0.00, 'N', 'O', DATE '1995-01-25', "
	    "DATE '1995-01-25', DATE '1995-01-25', 'NONE', 'AIR', 'c')");
	connection.Commit();
	accelerator_codegen::test_support::RequireAllPartitionsInvalid(connection, "hybrid_after_missing_part_insert", "1995-01-01");
	CheckQuery(connection, "hybrid_after_missing_part_insert_query", "1995-01-01");
}

void TestRollbackAndRyow() {
	accelerator_codegen::test_support::EnableHybridMaintenance();
	duckdb::DuckDB database(nullptr);
	duckdb::Connection writer(database);
	duckdb::Connection reader(database);
	CreateQ14Schema(writer);
	SeedBaseline(writer);
	accelerator_codegen::RegisterAcceleratorFunctions(writer);
	accelerator_codegen::RegisterAcceleratorFunctions(reader);
	BuildAux(writer);

	writer.BeginTransaction();
	writer.Query("INSERT INTO lineitem VALUES (3, 1, 1, 3, 1.00, 50.00, 0.00, 0.00, 'N', 'O', "
	             "DATE '1995-01-20', DATE '1995-01-20', DATE '1995-01-20', 'NONE', 'AIR', 'c')");
	CheckQuery(writer, "ryow_writer", "1995-01-01");
	CheckQuery(reader, "ryow_reader", "1995-01-01");
	writer.Rollback();

	accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(writer, "after_rollback", "1995-01-01");
	CheckQuery(writer, "after_rollback_query", "1995-01-01");
}

} // namespace

int main() {
	try {
		TestHybridRelevantBehavior();
		TestHybridIncrementalMaintenance();
		TestHybridIncrementalEdgeCases();
		TestRollbackAndRyow();
		std::cout << "test_q14_write_policies passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_q14_write_policies failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
