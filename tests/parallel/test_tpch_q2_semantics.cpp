#include "accelerator_codegen/register.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#ifndef ACCEL_ENABLE_PARALLEL_TPCH
#error "test_tpch_q2_semantics requires -DACCEL_ENABLE_PARALLEL_TPCH"
#endif

namespace {

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

std::string Escape(const std::string &value) {
	std::string out;
	for (char c : value) {
		if (c == '\'') {
			out += "''";
		} else {
			out.push_back(c);
		}
	}
	return out;
}

std::vector<std::string> MaterializeRows(duckdb::MaterializedQueryResult &result) {
	std::vector<std::string> rows;
	for (duckdb::idx_t row = 0; row < result.RowCount(); row++) {
		std::string encoded;
		for (duckdb::idx_t col = 0; col < result.ColumnCount(); col++) {
			if (col != 0) {
				encoded += " | ";
			}
			encoded += result.GetValue(col, row).ToString();
		}
		rows.push_back(std::move(encoded));
	}
	return rows;
}

void RequireEqualResults(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
                         const std::string &accelerator_sql) {
	auto expected = connection.Query(reference_sql);
	auto actual = connection.Query(accelerator_sql);
	Require(expected != nullptr && !expected->HasError(), label + " reference: " + expected->GetError());
	Require(actual != nullptr && !actual->HasError(), label + " accelerator: " + actual->GetError());
	auto &expected_m = expected->Cast<duckdb::MaterializedQueryResult>();
	auto &actual_m = actual->Cast<duckdb::MaterializedQueryResult>();
	Require(expected_m.RowCount() == actual_m.RowCount(), label + ": row count differs");
	Require(MaterializeRows(expected_m) == MaterializeRows(actual_m), label + ": rows differ");
}

void ResetTables(duckdb::Connection &connection) {
	connection.Query("DROP TABLE IF EXISTS partsupp");
	connection.Query("DROP TABLE IF EXISTS part");
	connection.Query("DROP TABLE IF EXISTS supplier");
	connection.Query("DROP TABLE IF EXISTS nation");
	connection.Query("DROP TABLE IF EXISTS region");
	Require(!connection.Query(
	                   "CREATE TABLE region(r_regionkey INTEGER, r_name VARCHAR);"
	                   "CREATE TABLE nation(n_nationkey INTEGER, n_name VARCHAR, n_regionkey INTEGER);"
	                   "CREATE TABLE supplier(s_suppkey BIGINT, s_name VARCHAR, s_address VARCHAR, s_nationkey INTEGER, "
	                   "s_phone VARCHAR, s_acctbal DECIMAL(15,2), s_comment VARCHAR);"
	                   "CREATE TABLE part(p_partkey BIGINT, p_mfgr VARCHAR, p_type VARCHAR, p_size INTEGER);"
	                   "CREATE TABLE partsupp(ps_partkey BIGINT, ps_suppkey BIGINT, ps_supplycost DECIMAL(15,2));")
	             ->HasError(),
	        "create schema");
}

void SeedBase(duckdb::Connection &connection) {
	connection.Query("INSERT INTO region VALUES (1, 'R1'), (2, 'R2')");
	connection.Query("INSERT INTO nation VALUES (10, 'N10', 1), (20, 'N20', 1), (30, 'N30', 2)");
	connection.Query(
	    "INSERT INTO supplier VALUES "
	    "(1, 'S1', 'A1', 10, 'P1', 100.00, 'C1'),"
	    "(2, 'S2', 'A2', 10, 'P2', 200.00, 'C2'),"
	    "(3, 'S3', 'A3', 20, 'P3', 150.00, 'C3'),"
	    "(4, 'S4', 'A4', 30, 'P4', 300.00, 'C4')");
	connection.Query("INSERT INTO part VALUES (100, 'M1', 'ECONOMY BRASS', 15), (101, 'M2', 'STEEL', 15)");
}

std::string ReferenceQ2(int32_t size, const std::string &type_pattern, const std::string &region) {
	return "SELECT s.s_acctbal, s.s_name, n.n_name, p.p_partkey, p.p_mfgr, s.s_address, s.s_phone, s.s_comment "
	       "FROM part p, supplier s, partsupp ps, nation n, region r "
	       "WHERE p.p_partkey = ps.ps_partkey AND s.s_suppkey = ps.ps_suppkey AND p.p_size = " +
	       std::to_string(size) + " AND p.p_type LIKE '" + Escape(type_pattern) +
	       "' AND s.s_nationkey = n.n_nationkey AND n.n_regionkey = r.r_regionkey AND r.r_name = '" + Escape(region) +
	       "' AND ps.ps_supplycost = (SELECT MIN(ps2.ps_supplycost) FROM partsupp ps2, supplier s2, nation n2, region r2 "
	       "WHERE p.p_partkey = ps2.ps_partkey AND s2.s_suppkey = ps2.ps_suppkey AND s2.s_nationkey = n2.n_nationkey "
	       "AND n2.n_regionkey = r2.r_regionkey AND r2.r_name = '" + Escape(region) +
	       "') ORDER BY s.s_acctbal DESC, n.n_name, s.s_name, p.p_partkey LIMIT 100";
}

std::string ParallelQ2(int32_t size, const std::string &type_pattern, const std::string &region) {
	return "SELECT * FROM accel_tpch_q2_parallel(" + std::to_string(size) + ", '" + Escape(type_pattern) + "', '" +
	       Escape(region) + "')";
}

void RunCase(duckdb::Connection &connection, const std::string &label, int32_t size, const std::string &type_pattern,
             const std::string &region, const std::function<void(duckdb::Connection &)> &setup) {
	ResetTables(connection);
	SeedBase(connection);
	setup(connection);
	RequireEqualResults(connection, label, ReferenceQ2(size, type_pattern, region), ParallelQ2(size, type_pattern, region));
}

} // namespace

int main() {
	try {
		duckdb::DuckDB database(":memory:");
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		connection.Query("PRAGMA threads=4");

		RunCase(connection, "type_suffix_match", 15, "%BRASS", "R1",
		        [](duckdb::Connection &c) { c.Query("INSERT INTO partsupp VALUES (100, 1, 50.00)"); });

		RunCase(connection, "size_mismatch", 10, "%BRASS", "R1",
		        [](duckdb::Connection &c) { c.Query("INSERT INTO partsupp VALUES (100, 1, 50.00)"); });

		RunCase(connection, "multiple_suppliers_tied_at_min", 15, "%BRASS", "R1", [](duckdb::Connection &c) {
			c.Query("INSERT INTO partsupp VALUES (100, 1, 50.00), (100, 2, 50.00), (100, 3, 75.00)");
		});

		RunCase(connection, "supplier_outside_region_excluded_from_min", 15, "%BRASS", "R1", [](duckdb::Connection &c) {
			c.Query("INSERT INTO partsupp VALUES (100, 1, 80.00), (100, 4, 10.00)");
		});

		RunCase(connection, "cost_above_minimum_excluded", 15, "%BRASS", "R1", [](duckdb::Connection &c) {
			c.Query("INSERT INTO partsupp VALUES (100, 1, 50.00), (100, 2, 60.00)");
		});

		RunCase(connection, "zero_rows", 15, "%TIN", "R1",
		        [](duckdb::Connection &c) { c.Query("INSERT INTO partsupp VALUES (100, 1, 50.00)"); });

		std::cout << "test_tpch_q2_semantics passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q2_semantics failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
