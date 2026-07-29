#include "accelerator_codegen/register.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#ifndef ACCEL_ENABLE_PARALLEL_TPCH
#error "test_tpch_q37_semantics requires -DACCEL_ENABLE_PARALLEL_TPCH"
#endif

namespace {

constexpr const char *kBrand = "Brand#11";
constexpr const char *kContainer = "SM CASE";

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

bool DoublesMatch(double expected, double actual) {
	if (std::isnan(expected) && std::isnan(actual)) {
		return std::signbit(expected) == std::signbit(actual);
	}
	if (std::isinf(expected) && std::isinf(actual)) {
		return std::signbit(expected) == std::signbit(actual);
	}
	uint64_t expected_bits = 0;
	uint64_t actual_bits = 0;
	std::memcpy(&expected_bits, &expected, sizeof(double));
	std::memcpy(&actual_bits, &actual, sizeof(double));
	return expected_bits == actual_bits;
}

std::string ReferenceQ17Sql() {
	return std::string(
	           "SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly "
	           "FROM lineitem, part, "
	           "(SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity "
	           "FROM lineitem GROUP BY l_partkey) part_agg "
	           "WHERE p_partkey = l_partkey AND agg_partkey = l_partkey "
	           "AND p_brand = '") +
	       kBrand + "' AND p_container = '" + kContainer + "' AND l_quantity < avg_quantity";
}

std::string AcceleratorQ17Sql() {
	return std::string("SELECT * FROM accel_tpch_q37('") + kBrand + "', '" + kContainer + "')";
}

std::string AcceleratorQ17ParallelSql() {
	return std::string("SELECT * FROM accel_tpch_q37_parallel('") + kBrand + "', '" + kContainer + "')";
}

void RequireEqualResults(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
                         const std::string &accelerator_sql) {
	auto expected = connection.Query(reference_sql);
	auto actual = connection.Query(accelerator_sql);
	Require(expected != nullptr && !expected->HasError(), label + " reference: " + expected->GetError());
	Require(actual != nullptr && !actual->HasError(), label + " accelerator: " + actual->GetError());
	auto &expected_m = expected->Cast<duckdb::MaterializedQueryResult>();
	auto &actual_m = actual->Cast<duckdb::MaterializedQueryResult>();
	Require(expected_m.RowCount() == 1 && actual_m.RowCount() == 1, label + ": row count differs");
	Require(expected_m.GetValue(0, 0).IsNull() == actual_m.GetValue(0, 0).IsNull(), label + ": nullability differs");
	if (expected_m.GetValue(0, 0).IsNull()) {
		return;
	}
	Require(DoublesMatch(expected_m.GetValue(0, 0).GetValue<double>(), actual_m.GetValue(0, 0).GetValue<double>()),
	        label + ": value differs");
}

void CreateQ37Schema(duckdb::Connection &connection) {
	auto result = connection.Query(
	    "CREATE TABLE part(p_partkey BIGINT NOT NULL, p_name VARCHAR NOT NULL, p_mfgr VARCHAR NOT NULL, "
	    "p_brand VARCHAR NOT NULL, p_type VARCHAR NOT NULL, p_size INTEGER NOT NULL, p_container VARCHAR NOT NULL, "
	    "p_retailprice DECIMAL(15,2) NOT NULL, p_comment VARCHAR NOT NULL);"
	    "CREATE TABLE lineitem(l_orderkey BIGINT NOT NULL, l_partkey BIGINT NOT NULL, l_suppkey BIGINT NOT NULL, "
	    "l_linenumber BIGINT NOT NULL, l_quantity DECIMAL(15,2) NOT NULL, l_extendedprice DECIMAL(15,2) NOT NULL, "
	    "l_discount DECIMAL(15,2) NOT NULL, l_tax DECIMAL(15,2) NOT NULL, l_returnflag VARCHAR NOT NULL, "
	    "l_linestatus VARCHAR NOT NULL, l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL, "
	    "l_receiptdate DATE NOT NULL, l_shipinstruct VARCHAR NOT NULL, l_shipmode VARCHAR NOT NULL, "
	    "l_comment VARCHAR NOT NULL);");
	Require(result != nullptr && !result->HasError(), result->GetError());
}

void InsertPart(duckdb::Connection &connection, int64_t partkey, const std::string &brand,
                const std::string &container) {
	connection.Query("INSERT INTO part VALUES (" + std::to_string(partkey) + ", 'n', 'm', '" + brand +
	                 "', 't', 1, '" + container + "', 10.00, 'c')");
}

void InsertLineitem(duckdb::Connection &connection, int64_t orderkey, int64_t partkey, int64_t lineno,
                    const char *quantity, const char *extendedprice) {
	connection.Query("INSERT INTO lineitem VALUES (" + std::to_string(orderkey) + ", " + std::to_string(partkey) +
	                 ", 1, " + std::to_string(lineno) + ", " + quantity + ", " + extendedprice +
	                 ", 0.00, 0.00, 'N', 'O', DATE '1995-01-01', DATE '1995-01-01', DATE '1995-01-01', 'N', 'A', 'c')");
}

struct SemanticFixture {
	std::string label;
	std::function<void(duckdb::Connection &)> setup;
};

void RunSemanticCase(duckdb::Connection &connection, const SemanticFixture &fixture) {
	connection.Query("DROP TABLE IF EXISTS lineitem");
	connection.Query("DROP TABLE IF EXISTS part");
	CreateQ37Schema(connection);
	fixture.setup(connection);
	const auto reference = ReferenceQ17Sql();
	RequireEqualResults(connection, fixture.label + "/serial", reference, AcceleratorQ17Sql());
	RequireEqualResults(connection, fixture.label + "/parallel", reference, AcceleratorQ17ParallelSql());
}

} // namespace

int main() {
	try {
		duckdb::DuckDB memory_database(nullptr);
		duckdb::Connection connection(memory_database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);

		const std::vector<SemanticFixture> fixtures = {
		    {"matching_brand_and_container",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, kBrand, kContainer);
			     InsertLineitem(c, 1, 1, 1, "10.00", "70.00");
			     InsertLineitem(c, 2, 1, 2, "30.00", "0.00");
		     }},
		    {"brand_only_no_match",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, kBrand, "LG BOX");
			     InsertLineitem(c, 1, 1, 1, "1.00", "100.00");
		     }},
		    {"container_only_no_match",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, "Brand#22", kContainer);
			     InsertLineitem(c, 1, 1, 1, "1.00", "100.00");
		     }},
		    {"neither_brand_nor_container",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, "Brand#22", "LG BOX");
			     InsertLineitem(c, 1, 1, 1, "1.00", "100.00");
		     }},
		    {"qualifying_part_no_lineitems",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, kBrand, kContainer);
		     }},
		    {"qualifying_part_one_lineitem",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, kBrand, kContainer);
			     InsertLineitem(c, 1, 1, 1, "10.00", "70.00");
		     }},
		    {"multiple_lineitems_one_part",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, kBrand, kContainer);
			     InsertLineitem(c, 1, 1, 1, "10.00", "70.00");
			     InsertLineitem(c, 2, 1, 2, "30.00", "0.00");
			     InsertLineitem(c, 3, 1, 3, "50.00", "0.00");
		     }},
		    {"multiple_qualifying_parts",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, kBrand, kContainer);
			     InsertPart(c, 2, kBrand, kContainer);
			     InsertLineitem(c, 1, 1, 1, "10.00", "70.00");
			     InsertLineitem(c, 2, 2, 1, "10.00", "35.00");
		     }},
		    {"exactly_at_threshold",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, kBrand, kContainer);
			     InsertLineitem(c, 1, 1, 1, "10.00", "70.00");
			     InsertLineitem(c, 2, 1, 2, "30.00", "0.00");
			     InsertLineitem(c, 3, 1, 3, "6.00", "99.00");
		     }},
		    {"just_below_threshold",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, kBrand, kContainer);
			     InsertLineitem(c, 1, 1, 1, "10.00", "70.00");
			     InsertLineitem(c, 2, 1, 2, "30.00", "0.00");
			     InsertLineitem(c, 3, 1, 3, "5.99", "99.00");
		     }},
		    {"just_above_threshold",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, kBrand, kContainer);
			     InsertLineitem(c, 1, 1, 1, "10.00", "70.00");
			     InsertLineitem(c, 2, 1, 2, "30.00", "0.00");
			     InsertLineitem(c, 3, 1, 3, "6.01", "99.00");
		     }},
		    {"duplicate_lineitems_affect_average",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, kBrand, kContainer);
			     InsertLineitem(c, 1, 1, 1, "10.00", "70.00");
			     InsertLineitem(c, 2, 1, 2, "10.00", "0.00");
			     InsertLineitem(c, 3, 1, 3, "30.00", "0.00");
		     }},
		    {"zero_quantity",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, kBrand, kContainer);
			     InsertLineitem(c, 1, 1, 1, "0.00", "70.00");
			     InsertLineitem(c, 2, 1, 2, "100.00", "0.00");
		     }},
		    {"decimal_quantity_and_price",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, kBrand, kContainer);
			     InsertLineitem(c, 1, 1, 1, "10.50", "123.45");
			     InsertLineitem(c, 2, 1, 2, "20.25", "0.00");
		     }},
		    {"division_by_seven",
		     [](duckdb::Connection &c) {
			     InsertPart(c, 1, kBrand, kContainer);
			     InsertLineitem(c, 1, 1, 1, "1.00", "70.00");
		     }},
		};

		for (const auto &fixture : fixtures) {
			RunSemanticCase(connection, fixture);
		}

		std::cout << "test_tpch_q37_semantics passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q37_semantics failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
