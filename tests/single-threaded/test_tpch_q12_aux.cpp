#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q12_aux.hpp"
#include "test_support/q12_aux_helpers.hpp"
#include "test_support/q12_manifest.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "duckdb.hpp"

namespace {

std::string GetTpchDbPath() {
	const char *env_path = std::getenv("ACCEL_TPCH_DB");
	return (env_path && *env_path) ? env_path : "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
}

void Require(bool condition, const std::string &message) {
	if (!condition) throw std::runtime_error(message);
}

void CreateQ12Schema(duckdb::Connection &connection) {
	auto result = connection.Query(
	    "CREATE TABLE orders(o_orderkey BIGINT NOT NULL, o_custkey BIGINT NOT NULL, o_orderstatus VARCHAR NOT NULL, "
	    "o_totalprice DECIMAL(15,2) NOT NULL, o_orderdate DATE NOT NULL, o_orderpriority VARCHAR NOT NULL, "
	    "o_clerk VARCHAR NOT NULL, o_shippriority INTEGER NOT NULL, o_comment VARCHAR NOT NULL);"
	    "CREATE TABLE lineitem(l_orderkey BIGINT NOT NULL, l_partkey BIGINT NOT NULL, l_suppkey BIGINT NOT NULL, "
	    "l_linenumber BIGINT NOT NULL, l_quantity DECIMAL(15,2) NOT NULL, l_extendedprice DECIMAL(15,2) NOT NULL, "
	    "l_discount DECIMAL(15,2) NOT NULL, l_tax DECIMAL(15,2) NOT NULL, l_returnflag VARCHAR NOT NULL, "
	    "l_linestatus VARCHAR NOT NULL, l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL, "
	    "l_receiptdate DATE NOT NULL, l_shipinstruct VARCHAR NOT NULL, l_shipmode VARCHAR NOT NULL, "
	    "l_comment VARCHAR NOT NULL);");
	Require(result != nullptr && !result->HasError(), result->GetError());
}

void CompareAux(duckdb::Connection &connection, const std::string &label, const std::string &shipmode1,
                const std::string &shipmode2, const std::string &start_date) {
	accelerator_codegen::test_support::RequireEqualQ12Results(
	    connection, label, accelerator_codegen::test_support::ReferenceQ12Sql(shipmode1, shipmode2, start_date),
	    accelerator_codegen::test_support::AcceleratorQ12AuxSql(shipmode1, shipmode2, start_date));
}

} // namespace

int main() {
	try {
		if (std::getenv("ACCEL_Q12_AUX_SKIP_SF1") == nullptr) {
			duckdb::DuckDB file_db(GetTpchDbPath().c_str());
			duckdb::Connection connection(file_db);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			connection.Query("PRAGMA threads=1");
			for (const auto &entry : accelerator_codegen::test_support::kQ12Manifest) {
				CompareAux(connection, std::string(entry.label) + "_before_build", entry.shipmode1, entry.shipmode2,
				           entry.start_date);
			}
			connection.Query("SELECT * FROM accel_q12_aux_build()");
			for (const auto &entry : accelerator_codegen::test_support::kQ12Manifest) {
				CompareAux(connection, std::string(entry.label) + "_after_build", entry.shipmode1, entry.shipmode2,
				           entry.start_date);
			}
		}

		duckdb::DuckDB memory_db(nullptr);
		duckdb::Connection connection(memory_db);
		CreateQ12Schema(connection);
		connection.Query("INSERT INTO orders VALUES (100, 1, 'O', 1000.00, DATE '1995-03-01', '1-URGENT', 'c', 0, 'c')");
		connection.Query("INSERT INTO lineitem VALUES (100, 1, 1, 1, 1.00, 100.00, 0.00, 0.00, 'N', 'O', "
		                 "DATE '1995-03-10', DATE '1995-03-15', DATE '1995-03-20', 'DELIVER IN PERSON', 'MAIL', 'c')");
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		connection.Query("SELECT * FROM accel_q12_aux_build()");
		CompareAux(connection, "synthetic", "MAIL", "SHIP", "1995-01-01");
		Require(accelerator_codegen::GetQ12AuxDiagnostics().used_aux_path, "expected aux fast path");

		connection.Query("INSERT INTO lineitem VALUES (100, 1, 1, 2, 1.00, 50.00, 0.00, 0.00, 'N', 'O', "
		                 "DATE '1995-03-10', DATE '1995-03-15', DATE '1995-03-25', 'DELIVER IN PERSON', 'MAIL', 'c')");
		CompareAux(connection, "after_lineitem_insert", "MAIL", "SHIP", "1995-01-01");

		std::cout << "test_tpch_q12_aux passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q12_aux failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
