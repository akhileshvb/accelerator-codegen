#include "accelerator_codegen/register.hpp"
#include "test_support/q19_manifest.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

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

std::string ReadFile(const std::string &path) {
	std::ifstream input(path);
	Require(input.good(), "failed to open file: " + path);
	std::ostringstream buffer;
	buffer << input.rdbuf();
	return buffer.str();
}

duckdb::Value QuerySingleValue(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Require(result != nullptr, "query returned null result");
	Require(!result->HasError(), result->GetError());
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	Require(materialized.RowCount() == 1, "expected a single-row result");
	return materialized.GetValue(0, 0);
}

std::string AcceleratorQ19Sql(const accelerator_codegen::test_support::Q19ManifestEntry &entry) {
	return std::string("SELECT * FROM accel_tpch_q19('") + entry.brand1 + "', " + std::to_string(entry.qty_low1) +
	       ", '" + entry.brand2 + "', " + std::to_string(entry.qty_low2) + ", '" + entry.brand3 + "', " +
	       std::to_string(entry.qty_low3) + ")";
}

void RequireEqualRevenue(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
                         const std::string &accelerator_sql) {
	auto expected = QuerySingleValue(connection, reference_sql);
	auto actual = QuerySingleValue(connection, accelerator_sql);
	Require(expected.type() == actual.type(), label + ": result types differ");
	Require(expected.IsNull() == actual.IsNull(), label + ": nullability differs");
	if (!expected.IsNull()) {
		Require(expected.GetValueUnsafe<duckdb::hugeint_t>() == actual.GetValueUnsafe<duckdb::hugeint_t>(),
		        label + ": decimal payload differs");
	}
}

void RequireQueryErrorContains(duckdb::Connection &connection, const std::string &label, const std::string &sql,
                               const std::string &needle) {
	auto result = connection.Query(sql);
	Require(result != nullptr, label + ": query returned null result");
	Require(result->HasError(), label + ": expected query to fail");
	Require(result->GetError().find(needle) != std::string::npos,
	        label + ": expected error to contain '" + needle + "' but saw: " + result->GetError());
}

void CreateQ39Schema(duckdb::Connection &connection, const std::string &partkey_type = "BIGINT") {
	auto result = connection.Query(
	    "CREATE TABLE part("
	    "p_partkey " +
	    partkey_type +
	    " NOT NULL, "
	    "p_name VARCHAR NOT NULL, "
	    "p_mfgr VARCHAR NOT NULL, "
	    "p_brand VARCHAR NOT NULL, "
	    "p_type VARCHAR NOT NULL, "
	    "p_size INTEGER NOT NULL, "
	    "p_container VARCHAR NOT NULL, "
	    "p_retailprice DECIMAL(15,2) NOT NULL, "
	    "p_comment VARCHAR NOT NULL);"
	    "CREATE TABLE lineitem("
	    "l_orderkey BIGINT NOT NULL, "
	    "l_partkey BIGINT NOT NULL, "
	    "l_suppkey BIGINT NOT NULL, "
	    "l_linenumber BIGINT NOT NULL, "
	    "l_quantity DECIMAL(15,2) NOT NULL, "
	    "l_extendedprice DECIMAL(15,2) NOT NULL, "
	    "l_discount DECIMAL(15,2) NOT NULL, "
	    "l_tax DECIMAL(15,2) NOT NULL, "
	    "l_returnflag VARCHAR NOT NULL, "
	    "l_linestatus VARCHAR NOT NULL, "
	    "l_shipdate DATE NOT NULL, "
	    "l_commitdate DATE NOT NULL, "
	    "l_receiptdate DATE NOT NULL, "
	    "l_shipinstruct VARCHAR NOT NULL, "
	    "l_shipmode VARCHAR NOT NULL, "
	    "l_comment VARCHAR NOT NULL);");
	Require(result != nullptr && !result->HasError(), "failed to create q19 schema: " + result->GetError());
}

void InsertPart(duckdb::Connection &connection, int64_t partkey, const std::string &brand, const std::string &container,
                int size) {
	auto result = connection.Query("INSERT INTO part VALUES (" + std::to_string(partkey) +
	                               ", 'name', 'Manufacturer#1', '" + brand + "', 'TYPE', " + std::to_string(size) +
	                               ", '" + container + "', 10.00, 'comment')");
	Require(result != nullptr && !result->HasError(), "failed to insert part: " + result->GetError());
}

void InsertLineitem(duckdb::Connection &connection, int64_t orderkey, int64_t partkey, int64_t linenumber,
                    const std::string &quantity, const std::string &extendedprice, const std::string &discount,
                    const std::string &shipmode, const std::string &shipinstruct) {
	auto result = connection.Query(
	    "INSERT INTO lineitem VALUES (" + std::to_string(orderkey) + ", " + std::to_string(partkey) + ", 1, " +
	    std::to_string(linenumber) + ", " + quantity + ", " + extendedprice + ", " + discount +
	    ", 0.00, 'N', 'O', DATE '1995-01-01', DATE '1995-01-01', DATE '1995-01-01', '" + shipinstruct + "', '" +
	    shipmode + "', 'comment')");
	Require(result != nullptr && !result->HasError(), "failed to insert lineitem: " + result->GetError());
}

void SetupSyntheticFixture(duckdb::Connection &connection) {
	CreateQ39Schema(connection);
	InsertPart(connection, 1, "Brand#31", "SM CASE", 3);
	InsertPart(connection, 2, "Brand#12", "MED BOX", 8);
	InsertPart(connection, 3, "Brand#51", "LG PACK", 12);

	// Branch 1 match: 100 * (1-0.10) = 90.00
	InsertLineitem(connection, 1, 1, 1, "5.00", "100.00", "0.10", "AIR", "DELIVER IN PERSON");
	// Branch 2 match: 50 * (1-0.00) = 50.00
	InsertLineitem(connection, 2, 2, 1, "16.00", "50.00", "0.00", "AIR REG", "DELIVER IN PERSON");
	// Branch 3 match: 25 * (1-0.20) = 20.00
	InsertLineitem(connection, 3, 3, 1, "24.00", "25.00", "0.20", "AIR", "DELIVER IN PERSON");
	// Wrong shipmode
	InsertLineitem(connection, 4, 1, 2, "5.00", "999.00", "0.00", "RAIL", "DELIVER IN PERSON");
	// Quantity below branch 1 lower bound
	InsertLineitem(connection, 5, 1, 3, "3.00", "888.00", "0.00", "AIR", "DELIVER IN PERSON");
}

void SetupTransactionFixture(duckdb::Connection &connection) {
	CreateQ39Schema(connection);
	InsertPart(connection, 10, "Brand#31", "SM CASE", 3);
	InsertLineitem(connection, 10, 10, 1, "5.00", "100.00", "0.00", "AIR", "DELIVER IN PERSON");
}

} // namespace

int main() {
	try {
		{
			duckdb::DuckDB file_database(GetTpchDbPath().c_str());
			duckdb::Connection connection(file_database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			auto pragma = connection.Query("PRAGMA threads=1");
			Require(pragma != nullptr && !pragma->HasError(), "failed PRAGMA threads=1");

			for (const auto &entry : accelerator_codegen::test_support::kQ39Manifest) {
				RequireEqualRevenue(connection, std::string(entry.label) + "_seed_file", ReadFile(entry.sql_path),
				                    AcceleratorQ19Sql(entry));
			}
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupSyntheticFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			const std::string reference =
			    "SELECT SUM(l_extendedprice *(1 - l_discount)) AS revenue FROM lineitem, part WHERE "
			    "(p_partkey = l_partkey AND p_brand = 'Brand#31' AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', "
			    "'SM PKG') AND l_quantity >= 4 AND l_quantity <= 14 AND p_size BETWEEN 1 AND 5 AND l_shipmode IN ('AIR', "
			    "'AIR REG') AND l_shipinstruct = 'DELIVER IN PERSON') OR "
			    "(p_partkey = l_partkey AND p_brand = 'Brand#12' AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', "
			    "'MED PACK') AND l_quantity >= 15 AND l_quantity <= 25 AND p_size BETWEEN 1 AND 10 AND l_shipmode IN "
			    "('AIR', 'AIR REG') AND l_shipinstruct = 'DELIVER IN PERSON') OR "
			    "(p_partkey = l_partkey AND p_brand = 'Brand#51' AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', "
			    "'LG PKG') AND l_quantity >= 23 AND l_quantity <= 33 AND p_size BETWEEN 1 AND 15 AND l_shipmode IN "
			    "('AIR', 'AIR REG') AND l_shipinstruct = 'DELIVER IN PERSON')";
			RequireEqualRevenue(connection, "synthetic_or_branches", reference,
			                    "SELECT * FROM accel_tpch_q19('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupTransactionFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
				const std::string accel = "SELECT * FROM accel_tpch_q19('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)";
			const std::string reference =
			    "SELECT SUM(l_extendedprice *(1 - l_discount)) AS revenue FROM lineitem, part WHERE "
			    "(p_partkey = l_partkey AND p_brand = 'Brand#31' AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', "
			    "'SM PKG') AND l_quantity >= 4 AND l_quantity <= 14 AND p_size BETWEEN 1 AND 5 AND l_shipmode IN ('AIR', "
			    "'AIR REG') AND l_shipinstruct = 'DELIVER IN PERSON') OR "
			    "(p_partkey = l_partkey AND p_brand = 'Brand#12' AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', "
			    "'MED PACK') AND l_quantity >= 15 AND l_quantity <= 25 AND p_size BETWEEN 1 AND 10 AND l_shipmode IN "
			    "('AIR', 'AIR REG') AND l_shipinstruct = 'DELIVER IN PERSON') OR "
			    "(p_partkey = l_partkey AND p_brand = 'Brand#51' AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', "
			    "'LG PKG') AND l_quantity >= 23 AND l_quantity <= 33 AND p_size BETWEEN 1 AND 15 AND l_shipmode IN "
			    "('AIR', 'AIR REG') AND l_shipinstruct = 'DELIVER IN PERSON')";

			RequireEqualRevenue(connection, "transaction_baseline", reference, accel);
			connection.BeginTransaction();
			InsertLineitem(connection, 11, 10, 2, "6.00", "40.00", "0.00", "AIR", "DELIVER IN PERSON");
			RequireEqualRevenue(connection, "after_insert", reference, accel);
			auto update = connection.Query("UPDATE lineitem SET l_shipmode = 'RAIL' WHERE l_orderkey = 10");
			Require(update != nullptr && !update->HasError(), update->GetError());
			RequireEqualRevenue(connection, "after_shipmode_update", reference, accel);
			connection.Rollback();
			RequireEqualRevenue(connection, "after_rollback", reference, accel);
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			CreateQ39Schema(connection, "INTEGER");
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			RequireQueryErrorContains(connection, "schema_mismatch",
			                          "SELECT * FROM accel_tpch_q19('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)",
			                          "Unsupported schema");
		}

		std::cout << "test_tpch_q39 passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q39 failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
