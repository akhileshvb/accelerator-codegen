#include "accelerator_codegen/register.hpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#ifndef ACCEL_ENABLE_PARALLEL_TPCH
#error "test_tpch_q20_semantics requires -DACCEL_ENABLE_PARALLEL_TPCH"
#endif

namespace {

constexpr const char *kNationName = "TESTNATION";
constexpr const char *kStartDate = "1995-06-01";
constexpr const char *kPartPattern = "testpart%";

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

std::vector<std::string> MaterializeOrderedRows(duckdb::MaterializedQueryResult &result) {
	std::vector<std::string> rows;
	rows.reserve(result.RowCount());
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

std::string ReferenceQ20Sql() {
	return std::string(
	           "SELECT s_name, s_address FROM supplier, nation "
	           "WHERE s_suppkey IN ("
	           "SELECT ps_suppkey FROM partsupp, ("
	           "SELECT l_partkey AS agg_partkey, l_suppkey AS agg_suppkey, 0.5 * SUM(l_quantity) AS agg_quantity "
	           "FROM lineitem WHERE l_shipdate >= DATE '") +
	       kStartDate +
	       "' AND l_shipdate < DATE '" + kStartDate +
	       "' + interval '1' year GROUP BY l_partkey, l_suppkey) agg "
	       "WHERE agg.agg_partkey = ps_partkey AND agg.agg_suppkey = ps_suppkey "
	       "AND ps_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE '" + kPartPattern + "') "
	       "AND ps_availqty > agg.agg_quantity) "
	       "AND s_nationkey = n_nationkey AND n_name = '" +
	       kNationName + "' ORDER BY s_name";
}

std::string AcceleratorQ20Sql() {
	return std::string("SELECT * FROM accel_tpch_q20(DATE '") + kStartDate + "', '" + kPartPattern + "', '" +
	       kNationName + "')";
}

std::string AcceleratorQ20ParallelSql() {
	return std::string("SELECT * FROM accel_tpch_q20_parallel(DATE '") + kStartDate + "', '" + kPartPattern + "', '" +
	       kNationName + "')";
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
	const auto expected_rows = MaterializeOrderedRows(expected_m);
	const auto actual_rows = MaterializeOrderedRows(actual_m);
	for (duckdb::idx_t row = 0; row < expected_rows.size(); row++) {
		Require(expected_rows[row] == actual_rows[row], label + ": row " + std::to_string(row) + " differs");
	}
}

void CreateQ20Schema(duckdb::Connection &connection) {
	auto result = connection.Query(
	    "CREATE TABLE part(p_partkey BIGINT NOT NULL, p_name VARCHAR NOT NULL, p_mfgr VARCHAR NOT NULL, "
	    "p_brand VARCHAR NOT NULL, p_type VARCHAR NOT NULL, p_size INTEGER NOT NULL, p_container VARCHAR NOT NULL, "
	    "p_retailprice DECIMAL(15,2) NOT NULL, p_comment VARCHAR NOT NULL);"
	    "CREATE TABLE supplier(s_suppkey BIGINT NOT NULL, s_name VARCHAR NOT NULL, s_address VARCHAR NOT NULL, "
	    "s_nationkey INTEGER NOT NULL, s_phone VARCHAR NOT NULL, s_acctbal DECIMAL(15,2) NOT NULL, s_comment VARCHAR NOT NULL);"
	    "CREATE TABLE nation(n_nationkey INTEGER NOT NULL, n_name VARCHAR NOT NULL, n_regionkey INTEGER NOT NULL, n_comment VARCHAR NOT NULL);"
	    "CREATE TABLE lineitem(l_orderkey BIGINT NOT NULL, l_partkey BIGINT NOT NULL, l_suppkey BIGINT NOT NULL, "
	    "l_linenumber BIGINT NOT NULL, l_quantity DECIMAL(15,2) NOT NULL, l_extendedprice DECIMAL(15,2) NOT NULL, "
	    "l_discount DECIMAL(15,2) NOT NULL, l_tax DECIMAL(15,2) NOT NULL, l_returnflag VARCHAR NOT NULL, "
	    "l_linestatus VARCHAR NOT NULL, l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL, l_receiptdate DATE NOT NULL, "
	    "l_shipinstruct VARCHAR NOT NULL, l_shipmode VARCHAR NOT NULL, l_comment VARCHAR NOT NULL);"
	    "CREATE TABLE partsupp(ps_partkey BIGINT NOT NULL, ps_suppkey BIGINT NOT NULL, ps_availqty BIGINT NOT NULL, "
	    "ps_supplycost DECIMAL(15,2) NOT NULL, ps_comment VARCHAR NOT NULL);");
	Require(result != nullptr && !result->HasError(), result->GetError());
	connection.Query("INSERT INTO nation VALUES (1, '" + std::string(kNationName) + "', 0, 'c')");
	connection.Query("INSERT INTO nation VALUES (2, 'OTHER', 0, 'c')");
}

void InsertPart(duckdb::Connection &connection, int64_t partkey, const std::string &name) {
	connection.Query("INSERT INTO part VALUES (" + std::to_string(partkey) + ", '" + name +
	                 "', 'm', 'b', 't', 1, 'c', 1.00, 'c')");
}

void InsertSupplier(duckdb::Connection &connection, int64_t suppkey, const std::string &name,
                    const std::string &address, int nationkey) {
	connection.Query("INSERT INTO supplier VALUES (" + std::to_string(suppkey) + ", '" + name + "', '" + address +
	                 "', " + std::to_string(nationkey) + ", 'phone', 0.00, 'c')");
}

void InsertPartsupp(duckdb::Connection &connection, int64_t partkey, int64_t suppkey, int64_t availqty) {
	connection.Query("INSERT INTO partsupp VALUES (" + std::to_string(partkey) + ", " + std::to_string(suppkey) + ", " +
	                 std::to_string(availqty) + ", 1.00, 'c')");
}

void InsertLineitem(duckdb::Connection &connection, int64_t orderkey, int64_t partkey, int64_t suppkey, int64_t lineno,
                    const char *shipdate, const char *quantity) {
	connection.Query("INSERT INTO lineitem VALUES (" + std::to_string(orderkey) + ", " + std::to_string(partkey) + ", " +
	                 std::to_string(suppkey) + ", " + std::to_string(lineno) + ", " + quantity +
	                 ", 1.00, 0.00, 0, 'N', 'O', DATE '" + shipdate +
	                 "', DATE '1995-01-01', DATE '1995-01-01', 'N', 'A', 'c')");
}

struct SemanticFixture {
	std::string label;
	std::function<void(duckdb::Connection &)> setup;
};

void RunSemanticCase(duckdb::Connection &connection, const SemanticFixture &fixture) {
	connection.Query("DROP TABLE IF EXISTS partsupp");
	connection.Query("DROP TABLE IF EXISTS lineitem");
	connection.Query("DROP TABLE IF EXISTS supplier");
	connection.Query("DROP TABLE IF EXISTS part");
	connection.Query("DROP TABLE IF EXISTS nation");
	CreateQ20Schema(connection);
	fixture.setup(connection);
	const auto reference = ReferenceQ20Sql();
	RequireEqualResults(connection, fixture.label + "/serial", reference, AcceleratorQ20Sql());
	RequireEqualResults(connection, fixture.label + "/parallel", reference, AcceleratorQ20ParallelSql());
}

} // namespace

int main() {
	try {
		duckdb::DuckDB memory_database(nullptr);
		duckdb::Connection connection(memory_database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);

		const std::vector<SemanticFixture> fixtures = {
		    {"no_matching_lineitem",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, "testpart_a");
			     InsertSupplier(c, 1, "qual_sup", "addr_a", 1);
			     InsertPartsupp(c, 1, 1, 1000);
		     }},
		    {"exactly_at_threshold",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, "testpart_a");
			     InsertSupplier(c, 1, "qual_sup", "addr_a", 1);
			     InsertPartsupp(c, 1, 1, 50);
			     InsertLineitem(c, 100, 1, 1, 1, "1995-07-01", "100.00");
		     }},
		    {"just_above_threshold",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, "testpart_a");
			     InsertSupplier(c, 1, "qual_sup", "addr_a", 1);
			     InsertPartsupp(c, 1, 1, 101);
			     InsertLineitem(c, 100, 1, 1, 1, "1995-07-01", "100.00");
		     }},
		    {"just_below_threshold",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, "testpart_a");
			     InsertSupplier(c, 1, "qual_sup", "addr_a", 1);
			     InsertPartsupp(c, 1, 1, 100);
			     InsertLineitem(c, 100, 1, 1, 1, "1995-07-01", "101.00");
		     }},
		    {"multiple_lineitems_same_pair",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, "testpart_a");
			     InsertSupplier(c, 1, "qual_sup", "addr_a", 1);
			     InsertPartsupp(c, 1, 1, 151);
			     InsertLineitem(c, 100, 1, 1, 1, "1995-07-01", "75.00");
			     InsertLineitem(c, 100, 1, 1, 2, "1995-08-01", "75.00");
		     }},
		    {"same_part_multiple_suppliers",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, "testpart_a");
			     InsertSupplier(c, 1, "qual_a", "addr_a", 1);
			     InsertSupplier(c, 2, "qual_b", "addr_b", 1);
			     InsertPartsupp(c, 1, 1, 101);
			     InsertPartsupp(c, 1, 2, 101);
			     InsertLineitem(c, 100, 1, 1, 1, "1995-07-01", "100.00");
			     InsertLineitem(c, 101, 1, 2, 1, "1995-07-01", "100.00");
		     }},
		    {"supplier_multiple_qualifying_parts",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, "testpart_a");
			     InsertPart(c, 2, "testpart_b");
			     InsertSupplier(c, 1, "qual_sup", "addr_a", 1);
			     InsertPartsupp(c, 1, 1, 101);
			     InsertPartsupp(c, 2, 1, 101);
			     InsertLineitem(c, 100, 1, 1, 1, "1995-07-01", "100.00");
			     InsertLineitem(c, 101, 2, 1, 1, "1995-07-01", "100.00");
		     }},
		    {"supplier_no_qualifying_parts",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, "testpart_a");
			     InsertSupplier(c, 1, "qual_sup", "addr_a", 1);
			     InsertPartsupp(c, 1, 1, 50);
			     InsertLineitem(c, 100, 1, 1, 1, "1995-07-01", "100.00");
		     }},
		    {"part_outside_prefix",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, "otherpart_a");
			     InsertSupplier(c, 1, "qual_sup", "addr_a", 1);
			     InsertPartsupp(c, 1, 1, 101);
			     InsertLineitem(c, 100, 1, 1, 1, "1995-07-01", "100.00");
		     }},
		    {"supplier_outside_nation",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, "testpart_a");
			     InsertSupplier(c, 1, "foreign_sup", "addr_x", 2);
			     InsertPartsupp(c, 1, 1, 101);
			     InsertLineitem(c, 100, 1, 1, 1, "1995-07-01", "100.00");
		     }},
		    {"boundary_dates",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, "testpart_a");
			     InsertSupplier(c, 1, "qual_sup", "addr_a", 1);
			     InsertPartsupp(c, 1, 1, 101);
			     InsertLineitem(c, 100, 1, 1, 1, "1995-06-01", "100.00");
			     InsertLineitem(c, 101, 1, 1, 2, "1996-05-31", "100.00");
			     InsertLineitem(c, 102, 1, 1, 3, "1996-06-01", "100.00");
		     }},
		    {"duplicate_lineitems_affect_sum",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, "testpart_a");
			     InsertSupplier(c, 1, "qual_sup", "addr_a", 1);
			     InsertPartsupp(c, 1, 1, 151);
			     InsertLineitem(c, 100, 1, 1, 1, "1995-07-01", "75.00");
			     InsertLineitem(c, 100, 1, 1, 2, "1995-07-01", "75.00");
		     }},
		    {"large_quantities",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, "testpart_a");
			     InsertSupplier(c, 1, "qual_sup", "addr_a", 1);
			     InsertPartsupp(c, 1, 1, 50000001);
			     InsertLineitem(c, 100, 1, 1, 1, "1995-07-01", "100000000.00");
		     }},
		    {"duplicate_partsupp_rows_same_supplier",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, "testpart_a");
			     InsertPart(c, 2, "testpart_b");
			     InsertSupplier(c, 1, "qual_sup", "addr_a", 1);
			     InsertPartsupp(c, 1, 1, 101);
			     InsertPartsupp(c, 2, 1, 101);
			     InsertLineitem(c, 100, 1, 1, 1, "1995-07-01", "100.00");
			     InsertLineitem(c, 101, 2, 1, 1, "1995-07-01", "100.00");
		     }},
		};

		for (const auto &fixture : fixtures) {
			RunSemanticCase(connection, fixture);
		}

		std::cout << "test_tpch_q20_semantics passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q20_semantics failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
