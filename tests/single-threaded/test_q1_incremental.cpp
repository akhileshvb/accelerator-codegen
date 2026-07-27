#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q1_aux.hpp"
#include "accelerator_codegen/tpch_q1_write_policy.hpp"
#include "test_support/q1_aux_helpers.hpp"

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

void CreateQ1Schema(duckdb::Connection &connection) {
	auto result = connection.Query(
	    "CREATE TABLE lineitem("
	    "l_orderkey BIGINT NOT NULL, l_partkey BIGINT NOT NULL, l_suppkey BIGINT NOT NULL, "
	    "l_linenumber BIGINT NOT NULL, l_quantity DECIMAL(15,2) NOT NULL, "
	    "l_extendedprice DECIMAL(15,2) NOT NULL, l_discount DECIMAL(15,2) NOT NULL, "
	    "l_tax DECIMAL(15,2) NOT NULL, l_returnflag VARCHAR NOT NULL, l_linestatus VARCHAR NOT NULL, "
	    "l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL, l_receiptdate DATE NOT NULL, "
	    "l_shipinstruct VARCHAR NOT NULL, l_shipmode VARCHAR NOT NULL, l_comment VARCHAR NOT NULL);");
	Require(result != nullptr && !result->HasError(), "failed to create q1 schema: " + result->GetError());
}

} // namespace

int main() {
	try {
		accelerator_codegen::test_support::EnableHybridMaintenance();
		duckdb::DuckDB database(nullptr);
		duckdb::Connection connection(database);
		CreateQ1Schema(connection);
		connection.Query("INSERT INTO lineitem VALUES (1,1,1,1,10.00,100.00,0.00,0.08,'N','O',"
		                 "DATE '1995-01-15',DATE '1995-01-15',DATE '1995-01-15','A','A','c')");
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		connection.Query("SELECT * FROM accel_q1_aux_build()");

		const auto day_offset = accelerator_codegen::test_support::DayOffsetForCutoff("1998-11-01");
		accelerator_codegen::test_support::RequireEqualQ1Results(connection, "baseline", day_offset);

		connection.BeginTransaction();
		connection.Query("INSERT INTO lineitem VALUES (2,1,1,2,5.00,50.00,0.00,0.08,'N','O',"
		                 "DATE '1995-01-20',DATE '1995-01-20',DATE '1995-01-20','A','A','c')");
		connection.Commit();
		accelerator_codegen::test_support::RequireEqualQ1Results(connection, "after_insert", day_offset);
		accelerator_codegen::test_support::RequireNoIncrementalFailures("after_insert");

		connection.BeginTransaction();
		connection.Query("UPDATE lineitem SET l_extendedprice = 75.00 WHERE l_orderkey = 2");
		connection.Commit();
		accelerator_codegen::test_support::RequireEqualQ1Results(connection, "after_price_update", day_offset);

		connection.BeginTransaction();
		connection.Query("UPDATE lineitem SET l_shipdate = DATE '1995-02-20' WHERE l_orderkey = 1");
		connection.Commit();
		accelerator_codegen::test_support::RequireEqualQ1Results(connection, "after_shipdate_move", day_offset);

		std::cout << "test_q1_incremental passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_q1_incremental failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
