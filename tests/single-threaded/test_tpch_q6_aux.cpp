#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q6_aux.hpp"
#include "test_support/q6_aux_helpers.hpp"
#include "test_support/q6_manifest.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {

std::string GetTpchDbPath() {
	const char *env_path = std::getenv("ACCEL_TPCH_DB");
	return (env_path && *env_path) ? env_path : "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
}

void Require(bool condition, const std::string &message) {
	if (!condition) throw std::runtime_error(message);
}

void CreateQ6Schema(duckdb::Connection &connection) {
	auto result = connection.Query(
	    "CREATE TABLE lineitem(l_orderkey BIGINT NOT NULL, l_partkey BIGINT NOT NULL, l_suppkey BIGINT NOT NULL, "
	    "l_linenumber BIGINT NOT NULL, l_quantity DECIMAL(15,2) NOT NULL, l_extendedprice DECIMAL(15,2) NOT NULL, "
	    "l_discount DECIMAL(15,2) NOT NULL, l_tax DECIMAL(15,2) NOT NULL, l_returnflag VARCHAR NOT NULL, "
	    "l_linestatus VARCHAR NOT NULL, l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL, "
	    "l_receiptdate DATE NOT NULL, l_shipinstruct VARCHAR NOT NULL, l_shipmode VARCHAR NOT NULL, "
	    "l_comment VARCHAR NOT NULL);"
	    "INSERT INTO lineitem VALUES (1, 10, 100, 1, 10.00, 100.00, 0.06, 0.01, 'N', 'O', DATE '1992-02-29', DATE '1992-03-01', DATE '1992-03-02', 'DELIVER', 'MAIL', 'ok');");
	Require(result != nullptr && !result->HasError(), result->GetError());
}

void CompareAux(duckdb::Connection &connection, const std::string &label, const std::string &start_date,
                const std::string &discount, const std::string &quantity) {
	accelerator_codegen::test_support::RequireEqualQ6Revenue(
	    connection, label, accelerator_codegen::test_support::ReferenceQ6Sql(start_date, discount, quantity),
	    accelerator_codegen::test_support::AcceleratorQ6AuxSql(start_date, discount, quantity));
}

} // namespace

int main() {
	try {
		if (std::getenv("ACCEL_Q6_AUX_SKIP_SF1") == nullptr) {
			duckdb::DuckDB file_db(GetTpchDbPath().c_str());
			duckdb::Connection connection(file_db);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			connection.Query("PRAGMA threads=1");
			for (const auto &entry : accelerator_codegen::test_support::kQ6Manifest) {
				CompareAux(connection, std::string(entry.label) + "_before_build", entry.start_date, entry.discount,
				           entry.quantity);
			}
			connection.Query("SELECT * FROM accel_q6_aux_build()");
			for (const auto &entry : accelerator_codegen::test_support::kQ6Manifest) {
				try {
					CompareAux(connection, std::string(entry.label) + "_after_build", entry.start_date, entry.discount,
					           entry.quantity);
				} catch (const std::exception &ex) {
					std::cerr << ex.what() << std::endl;
				}
			}
		}

		duckdb::DuckDB memory_db(nullptr);
		duckdb::Connection connection(memory_db);
		CreateQ6Schema(connection);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		connection.Query("SELECT * FROM accel_q6_aux_build()");
		CompareAux(connection, "synthetic", "1992-01-01", "0.06", "24.00");
		Require(accelerator_codegen::GetQ6AuxDiagnostics().used_aux_path, "expected aux fast path");

		connection.Query("INSERT INTO lineitem VALUES (2, 20, 200, 1, 20.00, 200.00, 0.06, 0.02, 'N', 'O', "
		                 "DATE '1992-06-01', DATE '1992-06-02', DATE '1992-06-03', 'DELIVER', 'MAIL', 'insert');");
		CompareAux(connection, "after_lineitem_insert", "1992-01-01", "0.06", "24.00");

		std::cout << "test_tpch_q6_aux passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q6_aux failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
