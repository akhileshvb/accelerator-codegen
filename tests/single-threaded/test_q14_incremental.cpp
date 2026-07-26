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

void InsertPart(duckdb::Connection &connection, int64_t partkey, const std::string &type) {
	auto result = connection.Query("INSERT INTO part VALUES (" + std::to_string(partkey) +
	                               ", 'name', 'Manufacturer#1', 'Brand#11', '" + type +
	                               "', 1, 'SM CASE', 10.00, 'comment')");
	Require(result != nullptr && !result->HasError(), "failed to insert part: " + result->GetError());
}

void InsertLineitem(duckdb::Connection &connection, int64_t orderkey, int64_t partkey, int64_t linenumber,
                    const std::string &extendedprice, const std::string &discount, const std::string &shipdate) {
	auto result = connection.Query(
	    "INSERT INTO lineitem VALUES (" + std::to_string(orderkey) + ", " + std::to_string(partkey) + ", 1, " +
	    std::to_string(linenumber) + ", 1.00, " + extendedprice + ", " + discount +
	    ", 0.00, 'N', 'O', DATE '" + shipdate + "', DATE '" + shipdate + "', DATE '" + shipdate +
	    "', 'NONE', 'AIR', 'comment')");
	Require(result != nullptr && !result->HasError(), "failed to insert lineitem: " + result->GetError());
}

} // namespace

int main() {
	try {
		accelerator_codegen::test_support::EnableHybridMaintenance();
		duckdb::DuckDB database(nullptr);
		duckdb::Connection connection(database);
		CreateQ14Schema(connection);
		InsertPart(connection, 1, "PROMO STEEL");
		InsertPart(connection, 2, "LARGE STEEL");
		InsertLineitem(connection, 1, 1, 1, "100.00", "0.00", "1995-01-15");
		InsertLineitem(connection, 2, 2, 1, "200.00", "0.00", "1995-01-15");
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		connection.Query("SELECT * FROM accel_q14_aux_build()");

		accelerator_codegen::test_support::RequireEqualQ14Results(
		    connection, "baseline", accelerator_codegen::test_support::ReferenceQ14Sql("1995-01-01"),
		    accelerator_codegen::test_support::AcceleratorQ14AuxSql("1995-01-01"));
		accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(connection, "baseline_partition",
		                                                                  "1995-01-01");

		connection.BeginTransaction();
		connection.Query("INSERT INTO lineitem VALUES (3, 1, 1, 3, 1.00, 50.00, 0.00, "
		                 "0.00, 'N', 'O', DATE '1995-01-20', DATE '1995-01-20', DATE '1995-01-20', "
		                 "'NONE', 'AIR', 'comment')");
		connection.Commit();
		accelerator_codegen::test_support::RequireEqualQ14Results(
		    connection, "after_insert", accelerator_codegen::test_support::ReferenceQ14Sql("1995-01-01"),
		    accelerator_codegen::test_support::AcceleratorQ14AuxSql("1995-01-01"));
		accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(connection, "after_insert_partition",
		                                                                  "1995-01-01");
		Require(accelerator_codegen::GetQ14AuxDiagnostics().used_aux_path,
		        "expected aux fast path after incremental insert");

		connection.BeginTransaction();
		connection.Query("UPDATE lineitem SET l_extendedprice = 75.00 WHERE l_orderkey = 3");
		connection.Commit();
		accelerator_codegen::test_support::RequireEqualQ14Results(
		    connection, "after_update", accelerator_codegen::test_support::ReferenceQ14Sql("1995-01-01"),
		    accelerator_codegen::test_support::AcceleratorQ14AuxSql("1995-01-01"));
		accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(connection, "after_update_partition",
		                                                                  "1995-01-01");
		Require(accelerator_codegen::GetQ14AuxDiagnostics().used_aux_path,
		        "expected aux fast path after incremental update");

		connection.BeginTransaction();
		connection.Query("UPDATE lineitem SET l_shipdate = DATE '1995-02-20' WHERE l_orderkey = 1");
		connection.Commit();
		accelerator_codegen::test_support::RequireEqualQ14Results(
		    connection, "after_shipdate_move_src", accelerator_codegen::test_support::ReferenceQ14Sql("1995-01-01"),
		    accelerator_codegen::test_support::AcceleratorQ14AuxSql("1995-01-01"));
		accelerator_codegen::test_support::RequireEqualQ14Results(
		    connection, "after_shipdate_move_dst", accelerator_codegen::test_support::ReferenceQ14Sql("1995-02-01"),
		    accelerator_codegen::test_support::AcceleratorQ14AuxSql("1995-02-01"));
		accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(connection, "after_shipdate_move_src_partition",
		                                                                  "1995-01-01");
		accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(connection, "after_shipdate_move_dst_partition",
		                                                                  "1995-02-01");

		connection.BeginTransaction();
		connection.Query("UPDATE part SET p_type = 'PROMO STEEL' WHERE p_partkey = 2");
		connection.Commit();
		accelerator_codegen::test_support::RequireEqualQ14Results(
		    connection, "after_part_type_change", accelerator_codegen::test_support::ReferenceQ14Sql("1995-02-01"),
		    accelerator_codegen::test_support::AcceleratorQ14AuxSql("1995-02-01"));
		accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(connection, "after_part_type_change_jan",
		                                                                  "1995-01-01");
		accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(connection, "after_part_type_change_feb",
		                                                                  "1995-02-01");
		Require(accelerator_codegen::GetQ14AuxDiagnostics().used_aux_path,
		        "expected aux fast path after incremental part p_type flip");
		accelerator_codegen::test_support::RequireNoIncrementalFailures("after_part_type_change");

		std::cout << "test_q14_incremental passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_q14_incremental failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
