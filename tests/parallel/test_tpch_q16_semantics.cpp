#include "accelerator_codegen/register.hpp"

#include <array>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#ifndef ACCEL_ENABLE_PARALLEL_TPCH
#error "test_tpch_q36_semantics requires -DACCEL_ENABLE_PARALLEL_TPCH"
#endif

namespace {

constexpr const char *kBrand = "B1";
constexpr const char *kTypePattern = "TEX%";
constexpr std::array<int32_t, 8> kSizes = {10, 20, 30, 40, 50, 60, 70, 80};

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
	Require(expected_rows == actual_rows, label + ": rows differ");
}

void ResetTables(duckdb::Connection &connection) {
	connection.Query("DROP TABLE IF EXISTS partsupp");
	connection.Query("DROP TABLE IF EXISTS part");
	connection.Query("DROP TABLE IF EXISTS supplier");
	Require(!connection
	             .Query("CREATE TABLE part(p_partkey BIGINT, p_brand VARCHAR, p_type VARCHAR, p_size INTEGER);"
	                    "CREATE TABLE supplier(s_suppkey BIGINT, s_comment VARCHAR);"
	                    "CREATE TABLE partsupp(ps_partkey BIGINT, ps_suppkey BIGINT);")
	             ->HasError(),
	        "create schema");
}

std::string ReferenceQ16() {
	std::string sizes;
	for (size_t i = 0; i < kSizes.size(); i++) {
		if (i != 0) {
			sizes += ", ";
		}
		sizes += std::to_string(kSizes[i]);
	}
	return "SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS supplier_cnt "
	       "FROM partsupp, part "
	       "WHERE p_partkey = ps_partkey "
	       "AND p_brand <> '" +
	       Escape(kBrand) + "' AND p_type NOT LIKE '" + Escape(kTypePattern) +
	       "' AND p_size IN (" + sizes +
	       ") AND ps_suppkey NOT IN (SELECT s_suppkey FROM supplier WHERE s_comment LIKE "
	       "'%Customer%Complaints%') "
	       "GROUP BY p_brand, p_type, p_size "
	       "ORDER BY supplier_cnt DESC, p_brand, p_type, p_size";
}

std::string ParallelQ16() {
	std::string sql = "SELECT * FROM accel_tpch_q16_parallel('" + Escape(kBrand) + "', '" + Escape(kTypePattern) + "'";
	for (const auto size : kSizes) {
		sql += ", " + std::to_string(size);
	}
	sql += ")";
	return sql;
}

void RunCase(duckdb::Connection &connection, const std::string &label,
             const std::function<void(duckdb::Connection &)> &setup) {
	ResetTables(connection);
	setup(connection);
	RequireEqualResults(connection, label, ReferenceQ16(), ParallelQ16());
}

} // namespace

int main() {
	try {
		duckdb::DuckDB database(":memory:");
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);

		RunCase(connection, "zero_qualifying_groups", [](duckdb::Connection &) {
		});

		RunCase(connection, "excluded_brand", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B1', 'OTHER', 10)");
			conn.Query("INSERT INTO supplier VALUES (1, 'ok')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1)");
		});

		RunCase(connection, "allowed_brand", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'OTHER', 10)");
			conn.Query("INSERT INTO supplier VALUES (1, 'ok')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1)");
		});

		RunCase(connection, "type_equals_prefix", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'TEX', 10)");
			conn.Query("INSERT INTO supplier VALUES (1, 'ok')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1)");
		});

		RunCase(connection, "type_starts_with_prefix", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'TEXAN', 10)");
			conn.Query("INSERT INTO supplier VALUES (1, 'ok')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1)");
		});

		RunCase(connection, "type_contains_prefix_later", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'ALTEX', 10)");
			conn.Query("INSERT INTO supplier VALUES (1, 'ok')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1)");
		});

		RunCase(connection, "type_no_prefix_match", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'OTHER', 10)");
			conn.Query("INSERT INTO supplier VALUES (1, 'ok')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1)");
		});

		RunCase(connection, "size_in_list", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'OTHER', 20)");
			conn.Query("INSERT INTO supplier VALUES (1, 'ok')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1)");
		});

		RunCase(connection, "size_not_in_list", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'OTHER', 99)");
			conn.Query("INSERT INTO supplier VALUES (1, 'ok')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1)");
		});

		RunCase(connection, "supplier_comment_match", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'OTHER', 10)");
			conn.Query("INSERT INTO supplier VALUES (1, 'Customer Complaints here')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1)");
		});

		RunCase(connection, "supplier_first_token_only", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'OTHER', 10)");
			conn.Query("INSERT INTO supplier VALUES (1, 'Customer only')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1)");
		});

		RunCase(connection, "supplier_second_token_only", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'OTHER', 10)");
			conn.Query("INSERT INTO supplier VALUES (1, 'Complaints only')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1)");
		});

		RunCase(connection, "supplier_tokens_reversed", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'OTHER', 10)");
			conn.Query("INSERT INTO supplier VALUES (1, 'Complaints before Customer')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1)");
		});

		RunCase(connection, "supplier_tokens_adjacent", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'OTHER', 10)");
			conn.Query("INSERT INTO supplier VALUES (2, 'CustomerComplaints')");
			conn.Query("INSERT INTO partsupp VALUES (1, 2)");
		});

		RunCase(connection, "supplier_tokens_separated", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'OTHER', 10)");
			conn.Query("INSERT INTO supplier VALUES (2, 'Customer and Complaints')");
			conn.Query("INSERT INTO partsupp VALUES (1, 2)");
		});

		RunCase(connection, "supplier_case_sensitive", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'OTHER', 10)");
			conn.Query("INSERT INTO supplier VALUES (1, 'customer complaints')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1)");
		});

		RunCase(connection, "one_supplier_one_part", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'OTHER', 10)");
			conn.Query("INSERT INTO supplier VALUES (1, 'ok')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1)");
		});

		RunCase(connection, "multiple_suppliers_one_part", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'OTHER', 10)");
			conn.Query("INSERT INTO supplier VALUES (1, 'ok'), (2, 'ok')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1), (1, 2)");
		});

		RunCase(connection, "shared_group_multiple_parts", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'OTHER', 10), (2, 'B2', 'OTHER', 10)");
			conn.Query("INSERT INTO supplier VALUES (1, 'ok'), (2, 'ok')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1), (2, 2)");
		});

		RunCase(connection, "same_supplier_multiple_parts_same_group", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'OTHER', 10), (2, 'B2', 'OTHER', 10)");
			conn.Query("INSERT INTO supplier VALUES (1, 'ok')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1), (2, 1)");
		});

		RunCase(connection, "duplicate_partsupp_rows", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'OTHER', 10)");
			conn.Query("INSERT INTO supplier VALUES (1, 'ok')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1), (1, 1)");
		});

		RunCase(connection, "same_supplier_multiple_groups", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'OTHER', 10), (2, 'B2', 'OTHER', 20)");
			conn.Query("INSERT INTO supplier VALUES (1, 'ok')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1), (2, 1)");
		});

		RunCase(connection, "group_only_excluded_suppliers", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'OTHER', 10)");
			conn.Query("INSERT INTO supplier VALUES (1, 'Customer Complaints')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1)");
		});

		RunCase(connection, "equal_supplier_counts_ordering", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'OTHER', 10), (2, 'B3', 'OTHER', 10)");
			conn.Query("INSERT INTO supplier VALUES (1, 'ok'), (2, 'ok')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1), (2, 2)");
		});

		RunCase(connection, "tie_ordering_brand_type_size", [](duckdb::Connection &conn) {
			conn.Query("INSERT INTO part VALUES (1, 'B2', 'AAA', 10), (2, 'B3', 'BBB', 20)");
			conn.Query("INSERT INTO supplier VALUES (1, 'ok'), (2, 'ok'), (3, 'ok')");
			conn.Query("INSERT INTO partsupp VALUES (1, 1), (1, 2), (2, 3)");
		});

		std::cout << "test_tpch_q36_semantics passed\n";
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q36_semantics failed: " << ex.what() << '\n';
		return 1;
	}
}
