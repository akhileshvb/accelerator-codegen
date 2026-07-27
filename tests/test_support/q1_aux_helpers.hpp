#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "accelerator_codegen/tpch_q1.hpp"
#include "accelerator_codegen/tpch_q1_aux.hpp"
#include "accelerator_codegen/tpch_q1_finalize.hpp"
#include "accelerator_codegen/tpch_q1_write_policy.hpp"
#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::test_support {

inline int32_t DayOffsetForCutoff(const std::string &cutoff_date) {
	const auto cutoff = duckdb::Date::FromString(cutoff_date);
	const auto base = duckdb::Date::FromDate(1998, 12, 1);
	return static_cast<int32_t>(base.days - cutoff.days);
}

inline std::string ReferenceQ1Sql(int32_t day_offset) {
	return "SELECT l_returnflag, l_linestatus, "
	       "sum(l_quantity) AS sum_qty, sum(l_extendedprice) AS sum_base_price, "
	       "sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price, "
	       "sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge, "
	       "avg(l_quantity) AS avg_qty, avg(l_extendedprice) AS avg_price, avg(l_discount) AS avg_disc, "
	       "count(*) AS count_order "
	       "FROM lineitem "
	       "WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '" +
	       std::to_string(day_offset) + "' DAY "
	       "GROUP BY l_returnflag, l_linestatus "
	       "ORDER BY l_returnflag, l_linestatus";
}

inline std::string AcceleratorQ1AuxSql(int32_t day_offset) {
	return "SELECT * FROM accel_tpch_q1_aux(" + std::to_string(day_offset) + ")";
}

inline std::string AcceleratorQ1Sql(int32_t day_offset) {
	return "SELECT * FROM accel_tpch_q1(" + std::to_string(day_offset) + ")";
}

inline std::string Q1DayPartitionTotalsSql(const std::string &shipdate) {
	return "SELECT count(*) AS row_count, "
	       "sum(l_quantity) AS sum_qty, sum(l_extendedprice) AS sum_base_price "
	       "FROM lineitem WHERE l_shipdate = DATE '" + shipdate + "'";
}

inline bool DoublesMatch(double expected, double actual) {
	if (std::isnan(expected) && std::isnan(actual)) {
		return std::signbit(expected) == std::signbit(actual);
	}
	if (std::isinf(expected) && std::isinf(actual)) {
		return std::signbit(expected) == std::signbit(actual);
	}
	uint64_t expected_bits = 0;
	uint64_t actual_bits = 0;
	static_assert(sizeof(double) == sizeof(uint64_t), "unexpected double size");
	std::memcpy(&expected_bits, &expected, sizeof(double));
	std::memcpy(&actual_bits, &actual, sizeof(double));
	return expected_bits == actual_bits;
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

inline void RequireEqualQ1Results(duckdb::Connection &connection, const std::string &label, int32_t day_offset) {
	auto expected = connection.Query(ReferenceQ1Sql(day_offset));
	auto actual = connection.Query(AcceleratorQ1AuxSql(day_offset));
	if (expected == nullptr || expected->HasError()) {
		throw std::runtime_error(label + ": reference failed: " + (expected ? expected->GetError() : "null"));
	}
	if (actual == nullptr || actual->HasError()) {
		throw std::runtime_error(label + ": accelerator failed: " + (actual ? actual->GetError() : "null"));
	}
	auto &expected_m = expected->Cast<duckdb::MaterializedQueryResult>();
	auto &actual_m = actual->Cast<duckdb::MaterializedQueryResult>();
	if (expected_m.RowCount() != actual_m.RowCount()) {
		throw std::runtime_error(label + ": row count differs, expected=" + std::to_string(expected_m.RowCount()) +
		                         " actual=" + std::to_string(actual_m.RowCount()));
	}
	for (duckdb::idx_t row = 0; row < expected_m.RowCount(); row++) {
		for (duckdb::idx_t col = 0; col < expected_m.ColumnCount(); col++) {
			if (!ValuesMatch(expected_m.GetValue(col, row), actual_m.GetValue(col, row))) {
				throw std::runtime_error(label + ": row " + std::to_string(row) + " col " + std::to_string(col) +
				                         " differs, expected=" + expected_m.GetValue(col, row).ToString() +
				                         " actual=" + actual_m.GetValue(col, row).ToString());
			}
		}
	}
}

inline void RequireAuxDayPartitionMatch(duckdb::Connection &connection, const std::string &label,
                                        const std::string &shipdate, bool expect_readable = true) {
	const auto partition = accelerator_codegen::PartitionKeyFromShipdate(duckdb::Date::FromString(shipdate));
	auto day = accelerator_codegen::TryGetReadableDayPartition(*connection.context, partition);
	if (expect_readable && !day) {
		throw std::runtime_error(label + ": expected readable aux partition for " + shipdate);
	}
	if (!expect_readable) {
		if (day) {
			throw std::runtime_error(label + ": expected aux partition to be invalid for " + shipdate);
		}
		return;
	}

	auto reference = connection.Query(Q1DayPartitionTotalsSql(shipdate));
	if (reference == nullptr || reference->HasError()) {
		throw std::runtime_error(label + ": partition totals reference failed: " +
		                         (reference ? reference->GetError() : "null"));
	}
	auto &reference_m = reference->Cast<duckdb::MaterializedQueryResult>();
	if (reference_m.RowCount() != 1) {
		throw std::runtime_error(label + ": expected single-row partition totals reference");
	}

	const auto ref_row_count = reference_m.GetValue(0, 0).GetValue<int64_t>();
	int64_t aux_row_count = 0;
	for (const auto &group : day->groups) {
		aux_row_count += group.second.count;
	}
	if (aux_row_count != ref_row_count) {
		throw std::runtime_error(label + ": row_count differs, aux=" + std::to_string(aux_row_count) +
		                         " sql=" + std::to_string(ref_row_count));
	}
}

inline void ClearQ1MaintenanceOverrides() {
	unsetenv("Q1_WRITE_POLICY");
	unsetenv("Q1_FORCE_ACTION");
	unsetenv("ACCEL_Q1_MAINTENANCE_MODE");
}

inline void EnableHybridMaintenance() {
	ClearQ1MaintenanceOverrides();
}

inline void RequireAllPartitionsInvalid(duckdb::Connection &connection, const std::string &label,
                                        const std::string &shipdate) {
	RequireAuxDayPartitionMatch(connection, label, shipdate, false);
}

inline void RequireNoIncrementalFailures(const std::string &label) {
	const auto &counters = GetQ1AuxCounters();
	if (counters.incremental_apply_failures != 0) {
		throw std::runtime_error(label + ": expected zero incremental_apply_failures, got " +
		                         std::to_string(counters.incremental_apply_failures));
	}
}

} // namespace accelerator_codegen::test_support
