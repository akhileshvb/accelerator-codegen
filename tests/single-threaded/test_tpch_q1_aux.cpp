#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q1_aux.hpp"
#include "test_support/q1_aux_helpers.hpp"
#include "test_support/q1_manifest.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "duckdb.hpp"

namespace {

std::string GetTpchDbPath() {
	const char *env_path = std::getenv("ACCEL_TPCH_DB");
	if (env_path && *env_path) {
		return env_path;
	}
	return "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
}

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

void InsertLineitem(duckdb::Connection &connection, int64_t orderkey, int64_t linenumber, const std::string &qty,
                    const std::string &extendedprice, const std::string &discount, const std::string &tax,
                    const std::string &returnflag, const std::string &linestatus, const std::string &shipdate) {
	auto result = connection.Query(
	    "INSERT INTO lineitem VALUES (" + std::to_string(orderkey) + ", 1, 1, " + std::to_string(linenumber) + ", " +
	    qty + ", " + extendedprice + ", " + discount + ", " + tax + ", '" + returnflag + "', '" + linestatus +
	    "', DATE '" + shipdate + "', DATE '" + shipdate + "', DATE '" + shipdate + "', 'NONE', 'AIR', 'comment')");
	Require(result != nullptr && !result->HasError(), "failed to insert lineitem: " + result->GetError());
}

void BuildAuxState(duckdb::Connection &connection) {
	auto result = connection.Query("SELECT * FROM accel_q1_aux_build()");
	Require(result != nullptr && !result->HasError(), result->GetError());
}

} // namespace

int main() {
	try {
		const char *db_env = std::getenv("ACCEL_TPCH_DB");
		const bool run_sf1 = std::getenv("ACCEL_Q1_AUX_SKIP_SF1") == nullptr && db_env != nullptr;
		if (run_sf1) {
			duckdb::DuckDB file_database(GetTpchDbPath().c_str());
			duckdb::Connection connection(file_database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			connection.Query("PRAGMA threads=1");

			for (const auto &entry : accelerator_codegen::test_support::kQ1Manifest) {
				accelerator_codegen::test_support::RequireEqualQ1Results(connection, entry.label, entry.day_offset);
			}

			BuildAuxState(connection);

			for (const auto &entry : accelerator_codegen::test_support::kQ1Manifest) {
				accelerator_codegen::test_support::RequireEqualQ1Results(connection,
				                                                         std::string(entry.label) + "_after_build",
				                                                         entry.day_offset);
			}
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			CreateQ1Schema(connection);
			InsertLineitem(connection, 1, 1, "10.00", "100.00", "0.05", "0.08", "N", "O", "1995-01-15");
			InsertLineitem(connection, 2, 1, "20.00", "200.00", "0.10", "0.07", "N", "O", "1995-02-15");
			InsertLineitem(connection, 3, 1, "5.00", "50.00", "0.00", "0.00", "R", "F", "1994-12-31");
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			BuildAuxState(connection);

			const auto day_offset = accelerator_codegen::test_support::DayOffsetForCutoff("1998-11-01");
			accelerator_codegen::test_support::RequireEqualQ1Results(connection, "synthetic", day_offset);
			accelerator_codegen::test_support::RequireAuxDayPartitionMatch(connection, "synthetic_partition",
			                                                               "1995-01-15");
		}

		std::cout << "test_tpch_q1_aux passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q1_aux failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
