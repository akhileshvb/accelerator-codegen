#include "backends/duckdb/api/register.hpp"
#include "tpch/q3/tests/q3_manifest.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {

std::string GetTpchDbPath() {
	const char *path = std::getenv("ACCEL_TPCH_DB");
	return path && *path ? path : "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
}

void Require(bool condition, const std::string &message) {
	if (!condition) throw std::runtime_error(message);
}

std::string ReadFile(const std::string &path) {
	std::ifstream input(path);
	Require(input.good(), "failed to open file: " + path);
	std::ostringstream contents;
	contents << input.rdbuf();
	return contents.str();
}

std::unique_ptr<duckdb::QueryResult> RunQuery(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Require(result != nullptr, "query returned null result");
	Require(!result->HasError(), result->GetError());
	return result;
}

std::vector<std::string> OrderedRows(duckdb::MaterializedQueryResult &result) {
	std::vector<std::string> rows;
	for (duckdb::idx_t row = 0; row < result.RowCount(); ++row) {
		std::string encoded;
		for (duckdb::idx_t column = 0; column < result.ColumnCount(); ++column) {
			if (column) encoded += " | ";
			encoded += result.GetValue(column, row).ToString();
		}
		rows.push_back(std::move(encoded));
	}
	return rows;
}

std::string EscapeLiteral(const std::string &value) {
	std::string escaped;
	for (char character : value) {
		if (character == '\'') escaped += "''";
		else escaped += character;
	}
	return escaped;
}

std::string AcceleratorSql(const std::string &segment, const std::string &date) {
	return "SELECT * FROM accel_tpch_q3('" + EscapeLiteral(segment) + "', DATE '" + date + "')";
}

std::string ReferenceSql(const std::string &segment, const std::string &date) {
	return "SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, o_orderdate, o_shippriority "
	       "FROM customer, orders, lineitem WHERE c_mktsegment = '" + EscapeLiteral(segment) +
	       "' AND c_custkey = o_custkey AND l_orderkey = o_orderkey AND o_orderdate < DATE '" + date +
	       "' AND l_shipdate > DATE '" + date +
	       "' GROUP BY l_orderkey, o_orderdate, o_shippriority ORDER BY revenue DESC, o_orderdate";
}

void Compare(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
	         const std::string &accelerator_sql) {
	auto expected = RunQuery(connection, reference_sql);
	auto actual = RunQuery(connection, accelerator_sql);
	auto &expected_rows = expected->Cast<duckdb::MaterializedQueryResult>();
	auto &actual_rows = actual->Cast<duckdb::MaterializedQueryResult>();
	Require(expected->names == actual->names, label + ": column names differ");
	Require(expected->types == actual->types, label + ": column types differ");
	Require(expected_rows.ColumnCount() == actual_rows.ColumnCount(), label + ": column counts differ");
	Require(expected_rows.RowCount() == actual_rows.RowCount(), label + ": row counts differ");
	Require(OrderedRows(expected_rows) == OrderedRows(actual_rows), label + ": ordered rows differ");
}

void SetupBoundaryFixture(duckdb::Connection &connection) {
	auto result = connection.Query(R"SQL(
CREATE TABLE customer(c_custkey BIGINT, c_name VARCHAR, c_address VARCHAR, c_nationkey BIGINT, c_phone VARCHAR, c_acctbal DECIMAL(15,2), c_mktsegment VARCHAR, c_comment VARCHAR);
CREATE TABLE orders(o_orderkey BIGINT, o_custkey BIGINT, o_orderstatus VARCHAR, o_totalprice DECIMAL(15,2), o_orderdate DATE, o_orderpriority VARCHAR, o_clerk VARCHAR, o_shippriority INTEGER, o_comment VARCHAR);
CREATE TABLE lineitem(l_orderkey BIGINT, l_partkey BIGINT, l_suppkey BIGINT, l_linenumber BIGINT, l_quantity DECIMAL(15,2), l_extendedprice DECIMAL(15,2), l_discount DECIMAL(15,2), l_tax DECIMAL(15,2), l_returnflag VARCHAR, l_linestatus VARCHAR, l_shipdate DATE, l_commitdate DATE, l_receiptdate DATE, l_shipinstruct VARCHAR, l_shipmode VARCHAR, l_comment VARCHAR);
INSERT INTO customer VALUES
 (1,'C1','',1,'',0.00,'BUILDING',''), (2,'C2','',1,'',0.00,'BUILDING',''), (3,'C3','',1,'',0.00,'FURNITURE','');
INSERT INTO orders VALUES
 (10,1,'O',0.00,DATE '1995-03-14','','',1,''), (20,2,'O',0.00,DATE '1995-03-15','','',2,''), (30,3,'O',0.00,DATE '1995-03-14','','',3,'');
INSERT INTO lineitem VALUES
 (10,1,1,1,1.00,100.00,0.10,0.00,'N','O',DATE '1995-03-16',DATE '1995-03-17',DATE '1995-03-18','','',''),
 (20,1,1,1,1.00,200.00,0.10,0.00,'N','O',DATE '1995-03-16',DATE '1995-03-17',DATE '1995-03-18','','',''),
 (10,1,1,2,1.00,50.00,0.20,0.00,'N','O',DATE '1995-03-15',DATE '1995-03-17',DATE '1995-03-18','','','');
)SQL");
	Require(result != nullptr, "boundary fixture returned null result");
	Require(!result->HasError(), result->GetError());
}

} // namespace

int main() {
	try {
		{
			duckdb::DuckDB database(GetTpchDbPath().c_str());
			duckdb::Connection connection(database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			for (const auto &entry : accelerator_codegen::test_support::kQ3Manifest) {
				Compare(connection, entry.label, ReadFile(entry.sql_path),
				        AcceleratorSql(entry.market_segment, entry.cutoff_date));
			}
		}
		{
			duckdb::DuckDB database(nullptr);
			duckdb::Connection connection(database);
			SetupBoundaryFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			Compare(connection, "in_memory_boundaries", ReferenceSql("BUILDING", "1995-03-15"),
			        AcceleratorSql("BUILDING", "1995-03-15"));
		}
		std::cout << "tpch_q3_smoke_test passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &exception) {
		std::cerr << "tpch_q3_smoke_test failed: " << exception.what() << std::endl;
		return EXIT_FAILURE;
	}
}
