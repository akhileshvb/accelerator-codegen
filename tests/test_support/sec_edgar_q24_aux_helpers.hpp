#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "accelerator_codegen/sec_edgar_q24_aux.hpp"
#include "accelerator_codegen/sec_edgar_q24_write_policy.hpp"
#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::test_support {

inline std::string ReferenceSecEdgarQ24Sql() {
	return "SELECT n.tag, n.version, COUNT(*) AS cnt, SUM(n.value) AS total "
	       "FROM num n "
	       "LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version AND n.adsh = p.adsh "
	       "WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 AND 20231231 "
	       "AND n.value IS NOT NULL AND p.adsh IS NULL "
	       "GROUP BY n.tag, n.version "
	       "HAVING COUNT(*) > 10 "
	       "ORDER BY cnt DESC "
	       "LIMIT 100";
}

inline std::string AcceleratorSecEdgarQ24AuxSql() {
	return "SELECT * FROM accel_sec_edgar_q24_aux()";
}

inline std::string AcceleratorSecEdgarQ24Sql() {
	return "SELECT * FROM accel_sec_edgar_q24()";
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

inline void RequireEqualSecEdgarQ24Results(duckdb::Connection &connection, const std::string &label) {
	auto expected = connection.Query(ReferenceSecEdgarQ24Sql());
	auto actual = connection.Query(AcceleratorSecEdgarQ24AuxSql());
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
	const auto &counters = GetSecEdgarQ24AuxCounters();
	if (counters.incremental_apply_failures > 0) {
		throw std::runtime_error(label + ": incremental apply failures=" +
		                         std::to_string(counters.incremental_apply_failures));
	}
}

} // namespace accelerator_codegen::test_support
