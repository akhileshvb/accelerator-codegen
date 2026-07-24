#include "accelerator_codegen/register.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#ifndef ACCEL_ENABLE_PARALLEL_TPCH
#error "test_tpch_q13_semantics requires -DACCEL_ENABLE_PARALLEL_TPCH"
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
	auto expected_rows = MaterializeRows(expected_m);
	auto actual_rows = MaterializeRows(actual_m);
	std::sort(expected_rows.begin(), expected_rows.end());
	std::sort(actual_rows.begin(), actual_rows.end());
	Require(expected_rows == actual_rows, label + ": rows differ");
}

void ResetTables(duckdb::Connection &connection) {
	connection.Query("DROP TABLE IF EXISTS orders");
	connection.Query("DROP TABLE IF EXISTS customer");
	Require(!connection.Query(
	                   "CREATE TABLE customer(c_custkey BIGINT);"
	                   "CREATE TABLE orders(o_orderkey BIGINT, o_custkey BIGINT, o_comment VARCHAR);")
	             ->HasError(),
	        "create schema");
}

void SeedCustomers(duckdb::Connection &connection) {
	connection.Query("INSERT INTO customer VALUES (1), (2), (3), (5)");
}

std::string ReferenceQ13(const std::string &pattern) {
	return "SELECT c_count, COUNT(*) AS custdist FROM (SELECT c_custkey, COUNT(o_orderkey) FROM customer "
	       "LEFT OUTER JOIN orders ON c_custkey = o_custkey AND o_comment NOT LIKE '" + Escape(pattern) +
	       "' GROUP BY c_custkey) AS c_orders(c_custkey, c_count) GROUP BY c_count ORDER BY custdist DESC, c_count DESC";
}

std::string ParallelQ13(const std::string &pattern) {
	return "SELECT * FROM accel_tpch_q13_parallel('" + Escape(pattern) + "')";
}

void RunCase(duckdb::Connection &connection, const std::string &label, const std::string &pattern,
             const std::function<void(duckdb::Connection &)> &setup) {
	ResetTables(connection);
	SeedCustomers(connection);
	setup(connection);
	RequireEqualResults(connection, label, ReferenceQ13(pattern), ParallelQ13(pattern));
}

} // namespace

int main() {
	try {
		duckdb::DuckDB database(":memory:");
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);

		const std::string pattern = "%foo%bar%";

		RunCase(connection, "zero_orders", pattern, [](duckdb::Connection &) {
		});

		RunCase(connection, "one_qualifying_order", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 1, 'ok comment')");
		});

		RunCase(connection, "multiple_qualifying_orders", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 1, 'ok')");
			conn.Query("INSERT INTO orders VALUES (2, 1, 'also ok')");
		});

		RunCase(connection, "only_disqualified_orders", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 1, 'has foo then bar here')");
		});

		RunCase(connection, "mixed_qualifying_and_disqualified", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 1, 'ok')");
			conn.Query("INSERT INTO orders VALUES (2, 1, 'foo and bar')");
		});

		RunCase(connection, "same_count_multiple_customers", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 1, 'ok')");
			conn.Query("INSERT INTO orders VALUES (2, 2, 'ok')");
		});

		RunCase(connection, "multiple_histogram_buckets", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 1, 'ok')");
			conn.Query("INSERT INTO orders VALUES (2, 2, 'ok')");
			conn.Query("INSERT INTO orders VALUES (3, 2, 'ok')");
		});

		RunCase(connection, "exact_pattern_match", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 1, 'prefix foo middle bar suffix')");
		});

		RunCase(connection, "first_word_only", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 1, 'foo only')");
		});

		RunCase(connection, "second_word_only", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 1, 'bar only')");
		});

		RunCase(connection, "reverse_word_order", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 1, 'bar then foo')");
		});

		RunCase(connection, "adjacent_words", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 1, 'foobarbaz')");
			conn.Query("INSERT INTO orders VALUES (2, 1, 'foo bar')");
		});

		RunCase(connection, "separated_words", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 1, 'foo x y z bar')");
		});

		RunCase(connection, "multiple_occurrences", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 1, 'foo foo bar bar')");
		});

		RunCase(connection, "words_at_edges", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 1, 'foo start')");
			conn.Query("INSERT INTO orders VALUES (2, 2, 'end bar')");
		});

		RunCase(connection, "similar_nonmatching", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 1, 'food barn')");
		});

		RunCase(connection, "case_sensitive", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 1, 'FOO bar')");
			conn.Query("INSERT INTO orders VALUES (2, 2, 'foo BAR')");
		});

		RunCase(connection, "empty_comment", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 1, '')");
		});

		RunCase(connection, "duplicate_orders", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 1, 'ok')");
			conn.Query("INSERT INTO orders VALUES (2, 1, 'ok')");
		});

		RunCase(connection, "customer_key_gaps", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 5, 'ok')");
		});

		RunCase(connection, "descending_sort", pattern, [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO orders VALUES (1, 1, 'ok')");
			conn.Query("INSERT INTO orders VALUES (2, 2, 'ok')");
			conn.Query("INSERT INTO orders VALUES (3, 2, 'ok')");
			conn.Query("INSERT INTO orders VALUES (4, 3, 'ok')");
		});

		std::cout << "test_tpch_q13_semantics passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q13_semantics failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
