#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q1_aux.hpp"
#include "test_support/q1_aux_helpers.hpp"

#include <iostream>

#include "duckdb.hpp"

int main() {
	try {
		accelerator_codegen::test_support::EnableHybridMaintenance();
		duckdb::DuckDB database(nullptr);
		duckdb::Connection connection(database);
		connection.Query(
		    "CREATE TABLE lineitem(l_orderkey BIGINT, l_partkey BIGINT, l_suppkey BIGINT, l_linenumber BIGINT, "
		    "l_quantity DECIMAL(15,2), l_extendedprice DECIMAL(15,2), l_discount DECIMAL(15,2), l_tax DECIMAL(15,2), "
		    "l_returnflag VARCHAR, l_linestatus VARCHAR, l_shipdate DATE, l_commitdate DATE, l_receiptdate DATE, "
		    "l_shipinstruct VARCHAR, l_shipmode VARCHAR, l_comment VARCHAR)");
		connection.Query("INSERT INTO lineitem VALUES (1,1,1,1,10,100,0,0,'N','O',DATE '1995-01-15',"
		                 "DATE '1995-01-15',DATE '1995-01-15','A','A','c'),"
		                 "(2,1,1,2,5,50,0,0,'N','O',DATE '1995-02-15',DATE '1995-02-15',DATE '1995-02-15','A','A','c')");
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		connection.Query("SELECT * FROM accel_q1_aux_build()");
		connection.Query("UPDATE lineitem SET l_shipdate = DATE '1995-03-15' WHERE l_orderkey = 1");
		const auto day_offset = accelerator_codegen::test_support::DayOffsetForCutoff("1998-11-01");
		accelerator_codegen::test_support::RequireEqualQ1Results(connection, "multi_partition_delta", day_offset);
		std::cout << "test_q1_multi_partition_delta passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_q1_multi_partition_delta failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
