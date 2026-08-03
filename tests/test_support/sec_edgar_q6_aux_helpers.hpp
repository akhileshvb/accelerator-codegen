#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "accelerator_codegen/sec_edgar_q6_aux.hpp"
#include "accelerator_codegen/sec_edgar_q6_write_policy.hpp"
#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::test_support {

inline std::string ReferenceSecEdgarQ6Sql() {
	return "SELECT s.name, p.stmt, n.tag, p.plabel, "
	       "SUM(n.value) AS total_value, COUNT(*) AS cnt "
	       "FROM num n "
	       "JOIN sub s ON n.adsh = s.adsh "
	       "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
	       "WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023 "
	       "AND n.value IS NOT NULL "
	       "GROUP BY s.name, p.stmt, n.tag, p.plabel "
	       "ORDER BY total_value DESC, s.name, n.tag, p.plabel "
	       "LIMIT 200";
}

inline std::string AcceleratorSecEdgarQ6AuxSql() {
	return "SELECT * FROM accel_sec_edgar_q6_aux()";
}

inline std::string AcceleratorSecEdgarQ6Sql() {
	return "SELECT * FROM accel_sec_edgar_q6()";
}

inline bool ApproxDoublesMatch(double expected, double actual) {
	if (std::isnan(expected) && std::isnan(actual)) {
		return std::signbit(expected) == std::signbit(actual);
	}
	if (std::isinf(expected) && std::isinf(actual)) {
		return std::signbit(expected) == std::signbit(actual);
	}
	const auto scale = std::max({1.0, std::abs(expected), std::abs(actual)});
	return std::abs(expected - actual) <= 1e-9 * scale;
}

inline bool ValuesMatch(const duckdb::Value &expected, const duckdb::Value &actual) {
	if (expected.IsNull() != actual.IsNull()) {
		return false;
	}
	if (expected.IsNull()) {
		return true;
	}
	if (expected.type().id() == duckdb::LogicalTypeId::DOUBLE) {
		return ApproxDoublesMatch(expected.GetValue<double>(), actual.GetValue<double>());
	}
	return expected.ToString() == actual.ToString();
}

inline void RequireEqualSecEdgarQ6Results(duckdb::Connection &connection, const std::string &label) {
	auto expected = connection.Query(ReferenceSecEdgarQ6Sql());
	auto actual = connection.Query(AcceleratorSecEdgarQ6AuxSql());
	if (expected == nullptr || expected->HasError()) {
		throw std::runtime_error(label + ": reference failed: " + (expected ? expected->GetError() : "null"));
	}
	if (actual == nullptr || actual->HasError()) {
		throw std::runtime_error(label + ": aux failed: " + (actual ? actual->GetError() : "null"));
	}
	auto &expected_mat = expected->Cast<duckdb::MaterializedQueryResult>();
	auto &actual_mat = actual->Cast<duckdb::MaterializedQueryResult>();
	if (expected_mat.RowCount() != actual_mat.RowCount()) {
		throw std::runtime_error(label + ": row count mismatch expected=" + std::to_string(expected_mat.RowCount()) +
		                         " actual=" + std::to_string(actual_mat.RowCount()));
	}
	for (duckdb::idx_t row = 0; row < expected_mat.RowCount(); row++) {
		for (duckdb::idx_t col = 0; col < expected_mat.ColumnCount(); col++) {
			if (!ValuesMatch(expected_mat.GetValue(col, row), actual_mat.GetValue(col, row))) {
				throw std::runtime_error(label + ": mismatch at row " + std::to_string(row) + " col " +
				                         std::to_string(col));
			}
		}
	}
}

inline void RequireNoIncrementalFailures(const std::string &label) {
	const auto &counters = GetSecEdgarQ6AuxCounters();
	if (counters.incremental_apply_failures > 0) {
		throw std::runtime_error(label + ": incremental apply failures=" +
		                         std::to_string(counters.incremental_apply_failures));
	}
}

} // namespace accelerator_codegen::test_support
