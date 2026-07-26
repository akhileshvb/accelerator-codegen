#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include "accelerator_codegen/tpch_q14_aux.hpp"
#include "accelerator_codegen/tpch_q14_write_policy.hpp"
#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::test_support {

inline std::string ReferenceQ14Sql(const std::string &start_date) {
	return "SELECT 100.00 * SUM(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * (1 - l_discount) ELSE 0 END) / "
	       "SUM(l_extendedprice * (1 - l_discount)) AS promo_revenue "
	       "FROM lineitem, part "
	       "WHERE l_partkey = p_partkey "
	       "AND l_shipdate >= DATE '" +
	       start_date + "' AND l_shipdate < DATE '" + start_date + "' + INTERVAL '1' month";
}

inline std::string AcceleratorQ14AuxSql(const std::string &start_date) {
	return "SELECT * FROM accel_tpch_q14_aux(DATE '" + start_date + "')";
}

inline std::string AcceleratorQ14Sql(const std::string &start_date) {
	return "SELECT * FROM accel_tpch_q14(DATE '" + start_date + "')";
}

inline std::string Q14PartitionTotalsSql(const std::string &start_date) {
	return "SELECT "
	       "SUM(l_extendedprice * (1 - l_discount)) AS total_revenue, "
	       "SUM(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * (1 - l_discount) ELSE 0 END) AS promo_revenue, "
	       "COUNT(*) AS row_count, "
	       "COUNT(*) > 0 AS saw_contribution "
	       "FROM lineitem, part "
	       "WHERE l_partkey = p_partkey "
	       "AND l_shipdate >= DATE '" +
	       start_date + "' AND l_shipdate < DATE '" + start_date + "' + INTERVAL '1' month";
}

inline double RawRevenueToDouble(Q14AccT raw) {
	return static_cast<double>(raw) / 10000.0;
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

inline void RequireEqualQ14Results(duckdb::Connection &connection, const std::string &label,
                                   const std::string &reference_sql, const std::string &accelerator_sql) {
	auto expected = connection.Query(reference_sql);
	auto actual = connection.Query(accelerator_sql);
	if (expected == nullptr || expected->HasError()) {
		throw std::runtime_error(label + ": reference failed: " + (expected ? expected->GetError() : "null"));
	}
	if (actual == nullptr || actual->HasError()) {
		throw std::runtime_error(label + ": accelerator failed: " + (actual ? actual->GetError() : "null"));
	}
	auto &expected_m = expected->Cast<duckdb::MaterializedQueryResult>();
	auto &actual_m = actual->Cast<duckdb::MaterializedQueryResult>();
	if (expected_m.RowCount() != 1 || actual_m.RowCount() != 1) {
		throw std::runtime_error(label + ": expected single-row results");
	}
	const auto expected_value = expected_m.GetValue(0, 0);
	const auto actual_value = actual_m.GetValue(0, 0);
	if (expected_value.IsNull() != actual_value.IsNull()) {
		throw std::runtime_error(label + ": nullability differs");
	}
	if (expected_value.IsNull()) {
		return;
	}
	const auto expected_d = expected_value.GetValue<double>();
	const auto actual_d = actual_value.GetValue<double>();
	if (!DoublesMatch(expected_d, actual_d)) {
		throw std::runtime_error(label + ": promo_revenue differs, expected=" + expected_value.ToString() +
		                         ", actual=" + actual_value.ToString());
	}
}

inline void RequireAuxPartitionTotalsMatch(duckdb::Connection &connection, const std::string &label,
                                             const std::string &start_date, bool expect_readable = true) {
	const auto partition = accelerator_codegen::PartitionKeyFromStartDate(duckdb::Date::FromString(start_date));
	auto totals = accelerator_codegen::TryGetReadableMonthTotals(*connection.context, partition);
	if (expect_readable && !totals) {
		throw std::runtime_error(label + ": expected readable aux partition for " + start_date);
	}
	if (!expect_readable) {
		if (totals) {
			throw std::runtime_error(label + ": expected aux partition to be invalid for " + start_date);
		}
		return;
	}

	auto reference = connection.Query(Q14PartitionTotalsSql(start_date));
	if (reference == nullptr || reference->HasError()) {
		throw std::runtime_error(label + ": partition totals reference failed: " +
		                         (reference ? reference->GetError() : "null"));
	}
	auto &reference_m = reference->Cast<duckdb::MaterializedQueryResult>();
	if (reference_m.RowCount() != 1) {
		throw std::runtime_error(label + ": expected single-row partition totals reference");
	}

	const auto ref_total = reference_m.GetValue(0, 0);
	const auto ref_promo = reference_m.GetValue(1, 0);
	const auto ref_row_count = reference_m.GetValue(2, 0);
	const auto ref_saw = reference_m.GetValue(3, 0);

	const bool ref_has_rows = !ref_row_count.IsNull() && ref_row_count.GetValue<int64_t>() > 0;
	if (totals->saw_contribution != ref_has_rows) {
		throw std::runtime_error(label + ": saw_contribution differs, aux=" +
		                         std::to_string(totals->saw_contribution) + " sql=" + std::to_string(ref_has_rows));
	}
	if (!ref_has_rows) {
		if (totals->row_count != 0) {
			throw std::runtime_error(label + ": expected aux row_count=0 for empty partition");
		}
		return;
	}

	const auto expected_rows = ref_row_count.GetValue<int64_t>();
	if (totals->row_count != expected_rows) {
		throw std::runtime_error(label + ": row_count differs, aux=" + std::to_string(totals->row_count) +
		                         " sql=" + std::to_string(expected_rows));
	}

	const auto expected_total = ref_total.GetValue<double>();
	const auto expected_promo = ref_promo.GetValue<double>();
	const auto actual_total = RawRevenueToDouble(totals->total_revenue_raw);
	const auto actual_promo = RawRevenueToDouble(totals->promo_revenue_raw);
	if (!DoublesMatch(expected_total, actual_total)) {
		throw std::runtime_error(label + ": total_revenue differs, expected=" + ref_total.ToString() +
		                         ", aux_raw=" + std::to_string(actual_total));
	}
	if (!DoublesMatch(expected_promo, actual_promo)) {
		throw std::runtime_error(label + ": promo_revenue differs, expected=" + ref_promo.ToString() +
		                         ", aux_raw=" + std::to_string(actual_promo));
	}
}

inline void ClearQ14MaintenanceOverrides() {
	unsetenv("Q14_WRITE_POLICY");
	unsetenv("Q14_FORCE_ACTION");
	unsetenv("ACCEL_Q14_MAINTENANCE_MODE");
}

inline void EnableHybridMaintenance() {
	ClearQ14MaintenanceOverrides();
}

inline void RequireAllPartitionsInvalid(duckdb::Connection &connection, const std::string &label,
                                      const std::string &start_date) {
	RequireAuxPartitionTotalsMatch(connection, label, start_date, false);
}

inline void RequireNoIncrementalFailures(const std::string &label) {
	const auto &counters = GetQ14AuxCounters();
	if (counters.incremental_apply_failures != 0) {
		throw std::runtime_error(label + ": expected zero incremental_apply_failures, got " +
		                         std::to_string(counters.incremental_apply_failures));
	}
}

} // namespace accelerator_codegen::test_support
