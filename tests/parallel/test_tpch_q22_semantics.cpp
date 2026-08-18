#include "accelerator_codegen/runtime/duckdb/register.hpp"

#include <array>
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
#error "test_tpch_q22_semantics requires -DACCEL_ENABLE_PARALLEL_TPCH"
#endif

namespace {

constexpr std::array<const char *, 7> kAccelCountryCodes = {"13", "30", "99", "98", "97", "96", "95"};
constexpr std::array<const char *, 2> kRefCountryCodes = {"13", "30"};

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

std::string ReferenceQ22Sql() {
	std::ostringstream in_list;
	for (size_t i = 0; i < kRefCountryCodes.size(); i++) {
		if (i != 0) {
			in_list << ", ";
		}
		in_list << "'" << kRefCountryCodes[i] << "'";
	}
	return std::string(
	           "WITH custsale AS ("
	           "SELECT SUBSTRING(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal "
	           "FROM customer "
	           "WHERE SUBSTRING(c_phone FROM 1 FOR 2) IN (") +
	       in_list.str() +
	       ") AND c_acctbal > (SELECT AVG(c_acctbal) FROM customer WHERE c_acctbal > 0.00 "
	       "AND SUBSTRING(c_phone FROM 1 FOR 2) IN (" +
	       in_list.str() +
	       ")) "
	       "AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey)) "
	       "SELECT cntrycode, COUNT(*) AS numcust, SUM(c_acctbal) AS totacctbal "
	       "FROM custsale GROUP BY cntrycode ORDER BY cntrycode";
}

std::string AcceleratorQ22Sql() {
	std::ostringstream stream;
	stream << "SELECT * FROM accel_tpch_q22(";
	for (size_t i = 0; i < kAccelCountryCodes.size(); i++) {
		if (i != 0) {
			stream << ", ";
		}
		stream << "'" << kAccelCountryCodes[i] << "'";
	}
	stream << ")";
	return stream.str();
}

std::string AcceleratorQ22ParallelSql() {
	std::ostringstream stream;
	stream << "SELECT * FROM accel_tpch_q22_parallel(";
	for (size_t i = 0; i < kAccelCountryCodes.size(); i++) {
		if (i != 0) {
			stream << ", ";
		}
		stream << "'" << kAccelCountryCodes[i] << "'";
	}
	stream << ")";
	return stream.str();
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

void CreateQ22Schema(duckdb::Connection &connection) {
	auto result = connection.Query(
	    "CREATE TABLE customer(c_custkey BIGINT NOT NULL, c_name VARCHAR NOT NULL, c_address VARCHAR NOT NULL, "
	    "c_nationkey INTEGER NOT NULL, c_phone VARCHAR NOT NULL, c_acctbal DECIMAL(15,2) NOT NULL, "
	    "c_mktsegment VARCHAR NOT NULL, c_comment VARCHAR NOT NULL);"
	    "CREATE TABLE orders(o_orderkey BIGINT NOT NULL, o_custkey BIGINT NOT NULL, o_orderstatus VARCHAR NOT NULL, "
	    "o_totalprice DECIMAL(15,2) NOT NULL, o_orderdate DATE NOT NULL, o_orderpriority VARCHAR NOT NULL, "
	    "o_clerk VARCHAR NOT NULL, o_shippriority INTEGER NOT NULL, o_comment VARCHAR NOT NULL);");
	Require(result != nullptr && !result->HasError(), result->GetError());
}

void InsertCustomer(duckdb::Connection &connection, int64_t custkey, const std::string &phone,
                    const std::string &acctbal) {
	connection.Query("INSERT INTO customer VALUES (" + std::to_string(custkey) + ", 'c" + std::to_string(custkey) +
	                 "', 'a', 1, '" + phone + "', " + acctbal + ", 's', 'c')");
}

void InsertOrder(duckdb::Connection &connection, int64_t orderkey, int64_t custkey) {
	connection.Query("INSERT INTO orders VALUES (" + std::to_string(orderkey) + ", " + std::to_string(custkey) +
	                 ", 'O', 1.00, DATE '1993-01-01', '1', 'c', 0, 'c')");
}

struct SemanticFixture {
	std::string label;
	std::function<void(duckdb::Connection &)> setup;
};

void RunSemanticCase(duckdb::Connection &connection, const SemanticFixture &fixture) {
	connection.Query("DROP TABLE IF EXISTS orders");
	connection.Query("DROP TABLE IF EXISTS customer");
	CreateQ22Schema(connection);
	fixture.setup(connection);
	const auto reference = ReferenceQ22Sql();
	RequireEqualResults(connection, fixture.label + "/serial", reference, AcceleratorQ22Sql());
	RequireEqualResults(connection, fixture.label + "/parallel", reference, AcceleratorQ22ParallelSql());
}

} // namespace

int main() {
	try {
		duckdb::DuckDB memory_database(nullptr);
		duckdb::Connection connection(memory_database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);

		const std::vector<SemanticFixture> fixtures = {
		    {"positive_balance_contributes_to_average",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "13-111-111-1111", "1000.00");
			     InsertCustomer(c, 2, "13-222-222-2222", "500.00");
			     InsertCustomer(c, 3, "30-333-333-3333", "2000.00");
			     InsertOrder(c, 10, 1);
		     }},
		    {"zero_balance_excluded_from_average",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "13-111-111-1111", "0.00");
			     InsertCustomer(c, 2, "13-222-222-2222", "1000.00");
			     InsertCustomer(c, 3, "30-333-333-3333", "3000.00");
		     }},
		    {"negative_balance_excluded_from_average",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "13-111-111-1111", "-100.00");
			     InsertCustomer(c, 2, "13-222-222-2222", "1000.00");
			     InsertCustomer(c, 3, "30-333-333-3333", "3000.00");
		     }},
		    {"outside_prefix_ignored",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "55-111-111-1111", "5000.00");
			     InsertCustomer(c, 2, "13-222-222-2222", "1000.00");
			     InsertCustomer(c, 3, "30-333-333-3333", "3000.00");
		     }},
		    {"exactly_at_average_rejected",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "13-111-111-1111", "1000.00");
			     InsertCustomer(c, 2, "13-222-222-2222", "2000.00");
		     }},
		    {"just_above_average_qualifies",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "13-111-111-1111", "1000.00");
			     InsertCustomer(c, 2, "13-222-222-2222", "2000.00");
			     InsertCustomer(c, 3, "13-333-333-3333", "2000.01");
		     }},
		    {"just_below_average_rejected",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "13-111-111-1111", "1000.00");
			     InsertCustomer(c, 2, "13-222-222-2222", "2000.00");
			     InsertCustomer(c, 3, "13-333-333-3333", "1999.99");
		     }},
		    {"customer_with_one_order_excluded_from_output",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "13-111-111-1111", "5000.00");
			     InsertCustomer(c, 2, "13-222-222-2222", "1000.00");
			     InsertOrder(c, 10, 1);
		     }},
		    {"customer_with_multiple_orders",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "13-111-111-1111", "5000.00");
			     InsertCustomer(c, 2, "13-222-222-2222", "1000.00");
			     InsertOrder(c, 10, 1);
			     InsertOrder(c, 11, 1);
		     }},
		    {"customer_without_orders_qualifies",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "13-111-111-1111", "1000.00");
			     InsertCustomer(c, 2, "13-222-222-2222", "5000.00");
		     }},
		    {"multiple_qualifying_same_prefix",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "13-111-111-1111", "1000.00");
			     InsertCustomer(c, 2, "13-222-222-2222", "5000.00");
			     InsertCustomer(c, 3, "13-333-333-3333", "6000.00");
		     }},
		    {"qualifying_across_multiple_prefixes",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "13-111-111-1111", "1000.00");
			     InsertCustomer(c, 2, "13-222-222-2222", "5000.00");
			     InsertCustomer(c, 3, "30-333-333-3333", "1000.00");
			     InsertCustomer(c, 4, "30-444-444-4444", "5000.00");
		     }},
		    {"prefix_in_query_with_zero_output",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "13-111-111-1111", "1000.00");
			     InsertCustomer(c, 2, "13-222-222-2222", "5000.00");
		     }},
		    {"tpch_phone_format",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "13-749-946-6340", "1000.00");
			     InsertCustomer(c, 2, "30-895-746-3948", "1000.00");
			     InsertCustomer(c, 3, "30-895-746-3949", "5000.00");
		     }},
		    {"ordered_customer_with_order_still_in_average",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "13-111-111-1111", "3000.00");
			     InsertCustomer(c, 2, "13-222-222-2222", "1000.00");
			     InsertCustomer(c, 3, "13-333-333-3333", "5000.00");
			     InsertOrder(c, 10, 1);
		     }},
		    {"decimal_balance_accumulation",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "13-111-111-1111", "1000.33");
			     InsertCustomer(c, 2, "13-222-222-2222", "2000.67");
			     InsertCustomer(c, 3, "13-333-333-3333", "4000.01");
		     }},
		    {"short_phone_rejected",
		     [](duckdb::Connection &c) {
			     InsertCustomer(c, 1, "1", "5000.00");
			     InsertCustomer(c, 2, "13-222-222-2222", "1000.00");
			     InsertCustomer(c, 3, "13-333-333-3333", "5000.00");
		     }},
		};

		for (const auto &fixture : fixtures) {
			RunSemanticCase(connection, fixture);
		}

		std::cout << "test_tpch_q22_semantics passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q22_semantics failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
