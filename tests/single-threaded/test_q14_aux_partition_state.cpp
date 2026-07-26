#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q14_aux.hpp"
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

void CheckAuxMonth(duckdb::Connection &connection, const std::string &label, const std::string &start_date,
                   bool expect_readable = true) {
	accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(connection, label, start_date, expect_readable);
	accelerator_codegen::test_support::RequireEqualQ14Results(
	    connection, label + "_query", accelerator_codegen::test_support::ReferenceQ14Sql(start_date),
	    accelerator_codegen::test_support::AcceleratorQ14AuxSql(start_date));
}

} // namespace

int main() {
	try {
		accelerator_codegen::test_support::EnableHybridMaintenance();
		duckdb::DuckDB database(nullptr);
		duckdb::Connection connection(database);
		CreateQ14Schema(connection);
		connection.Query("INSERT INTO part VALUES (1, 'n', 'm', 'b', 'PROMO STEEL', 1, 'SM', 10.00, 'c'), "
		                 "(2, 'n', 'm', 'b', 'LARGE STEEL', 1, 'SM', 10.00, 'c')");
		connection.Query("INSERT INTO lineitem VALUES "
		                 "(1, 1, 1, 1, 1.00, 100.00, 0.00, 0.00, 'N', 'O', DATE '1995-01-15', DATE '1995-01-15', "
		                 "DATE '1995-01-15', 'NONE', 'AIR', 'c'), "
		                 "(2, 2, 1, 1, 1.00, 200.00, 0.00, 0.00, 'N', 'O', DATE '1995-01-15', DATE '1995-01-15', "
		                 "DATE '1995-01-15', 'NONE', 'AIR', 'c')");
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		connection.Query("SELECT * FROM accel_q14_aux_build()");

		CheckAuxMonth(connection, "baseline_jan", "1995-01-01");

		connection.BeginTransaction();
		connection.Query("INSERT INTO lineitem VALUES (3, 1, 1, 3, 1.00, 50.00, 0.00, 0.00, 'N', 'O', "
		                 "DATE '1995-01-20', DATE '1995-01-20', DATE '1995-01-20', 'NONE', 'AIR', 'c')");
		connection.Commit();
		CheckAuxMonth(connection, "after_insert", "1995-01-01");

		connection.BeginTransaction();
		connection.Query("UPDATE lineitem SET l_extendedprice = 75.00 WHERE l_orderkey = 3");
		connection.Commit();
		CheckAuxMonth(connection, "after_price_update", "1995-01-01");

		connection.BeginTransaction();
		connection.Query("UPDATE lineitem SET l_shipdate = DATE '1995-02-20' WHERE l_orderkey = 1");
		connection.Commit();
		CheckAuxMonth(connection, "after_shipdate_move_src", "1995-01-01");
		CheckAuxMonth(connection, "after_shipdate_move_dst", "1995-02-01");

		connection.BeginTransaction();
		connection.Query("UPDATE part SET p_type = 'PROMO STEEL' WHERE p_partkey = 2");
		connection.Commit();
		CheckAuxMonth(connection, "after_part_type_flip_jan", "1995-01-01");
		CheckAuxMonth(connection, "after_part_type_flip_feb", "1995-02-01");
		Require(accelerator_codegen::GetQ14AuxDiagnostics().used_aux_path,
		        "expected aux path after hybrid part p_type flip");

		std::cout << "test_q14_aux_partition_state passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_q14_aux_partition_state failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
