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

void CreateSchema(duckdb::Connection &connection) {
	connection.Query(
	    "CREATE TABLE part(p_partkey BIGINT, p_type VARCHAR);"
	    "CREATE TABLE lineitem(l_orderkey BIGINT, l_partkey BIGINT, l_suppkey BIGINT, l_linenumber BIGINT, "
	    "l_quantity DECIMAL(15,2), l_extendedprice DECIMAL(15,2), l_discount DECIMAL(15,2), l_tax DECIMAL(15,2), "
	    "l_returnflag VARCHAR, l_linestatus VARCHAR, l_shipdate DATE, l_commitdate DATE, l_receiptdate DATE, "
	    "l_shipinstruct VARCHAR, l_shipmode VARCHAR, l_comment VARCHAR)");
}

void TestHybridPointLineitem() {
	accelerator_codegen::test_support::EnableHybridMaintenance();
	duckdb::DuckDB database(nullptr);
	duckdb::Connection connection(database);
	CreateSchema(connection);
	connection.Query("INSERT INTO part VALUES (1, 'PROMO STEEL')");
	connection.Query("INSERT INTO lineitem VALUES (1,1,1,1,1,100,0,0,'N','O',DATE '1995-01-15',DATE '1995-01-15',"
	                 "DATE '1995-01-15','A','A','c')");
	accelerator_codegen::RegisterAcceleratorFunctions(connection);
	connection.Query("SELECT * FROM accel_q14_aux_build()");
	connection.Query("UPDATE lineitem SET l_extendedprice = 80.00 WHERE l_orderkey = 1");
	accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(connection, "hybrid_point_update", "1995-01-01");
	Require(accelerator_codegen::GetQ14AuxCounters().action_selected_apply_delta >= 1,
	        "expected hybrid to select ApplyDelta");
}

void TestHybridPointPartTypeFlip() {
	accelerator_codegen::test_support::EnableHybridMaintenance();
	duckdb::DuckDB database(nullptr);
	duckdb::Connection connection(database);
	CreateSchema(connection);
	connection.Query("INSERT INTO part VALUES (1, 'PROMO STEEL'), (2, 'LARGE STEEL')");
	connection.Query("INSERT INTO lineitem VALUES "
	                 "(1,1,1,1,1,100,0,0,'N','O',DATE '1995-01-15',DATE '1995-01-15',DATE '1995-01-15','A','A','c'),"
	                 "(2,1,1,2,1,80,0,0,'N','O',DATE '1995-02-15',DATE '1995-02-15',DATE '1995-02-15','A','A','c')");
	accelerator_codegen::RegisterAcceleratorFunctions(connection);
	connection.Query("SELECT * FROM accel_q14_aux_build()");
	accelerator_codegen::ResetQ14AuxCounters();
	connection.Query("UPDATE part SET p_type = 'LARGE STEEL' WHERE p_partkey = 1");
	accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(connection, "hybrid_point_part_flip", "1995-01-01");
	Require(accelerator_codegen::GetQ14AuxCounters().action_selected_apply_delta >= 1,
	        "expected hybrid to select ApplyDelta for point part type flip");
}

void TestHybridIrrelevantCommentNoOp() {
	accelerator_codegen::test_support::EnableHybridMaintenance();
	duckdb::DuckDB database(nullptr);
	duckdb::Connection connection(database);
	CreateSchema(connection);
	connection.Query("INSERT INTO part VALUES (1, 'PROMO STEEL')");
	connection.Query("INSERT INTO lineitem VALUES (1,1,1,1,1,100,0,0,'N','O',DATE '1995-01-15',DATE '1995-01-15',"
	                 "DATE '1995-01-15','A','A','c')");
	accelerator_codegen::RegisterAcceleratorFunctions(connection);
	connection.Query("SELECT * FROM accel_q14_aux_build()");
	accelerator_codegen::ResetQ14AuxCounters();
	connection.Query("UPDATE lineitem SET l_comment='x' WHERE l_orderkey=1");
	const auto &counters = accelerator_codegen::GetQ14AuxCounters();
	Require(counters.hybrid_decisions == 0, "irrelevant comment update should not run hybrid maintenance");
	Require(counters.action_selected_invalidate == 0, "irrelevant comment update must not invalidate");
}

} // namespace

int main() {
	try {
		TestHybridPointLineitem();
		TestHybridPointPartTypeFlip();
		TestHybridIrrelevantCommentNoOp();
		std::cout << "test_q14_hybrid_policy passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_q14_hybrid_policy failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
