#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "accelerator_codegen/sec_edgar_q1_aux.hpp"
#include "accelerator_codegen/sec_edgar_q1_write_policy.hpp"
#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::test_support {

inline std::string ReferenceSecEdgarQ1Sql() {
	return "SELECT stmt, rfile, COUNT(*) AS cnt, COUNT(DISTINCT adsh) AS num_filings, AVG(line) AS avg_line_num "
	       "FROM pre WHERE stmt IS NOT NULL GROUP BY stmt, rfile ORDER BY cnt DESC";
}

inline std::string AcceleratorSecEdgarQ1AuxSql() {
	return "SELECT * FROM accel_sec_edgar_q1_aux()";
}

inline std::string AcceleratorSecEdgarQ1Sql() {
	return "SELECT * FROM accel_sec_edgar_q1()";
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

inline void RequireEqualSecEdgarQ1Results(duckdb::Connection &connection, const std::string &label) {
	auto expected = connection.Query(ReferenceSecEdgarQ1Sql());
	auto actual = connection.Query(AcceleratorSecEdgarQ1AuxSql());
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

inline void EnableHybridMaintenance() {
	setenv("ACCEL_AUX_MAINTENANCE_SELECTOR", "hybrid", 1);
}

inline void RequireNoIncrementalFailures(const std::string &label) {
	const auto &counters = GetSecEdgarQ1AuxCounters();
	if (counters.incremental_apply_failures > 0) {
		throw std::runtime_error(label + ": incremental apply failures=" +
		                         std::to_string(counters.incremental_apply_failures));
	}
}

} // namespace accelerator_codegen::test_support
