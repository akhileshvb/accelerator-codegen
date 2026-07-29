#pragma once

#include <stdexcept>
#include <string>

#include "accelerator_codegen/tpch_q3_aux.hpp"
#include "accelerator_codegen/tpch_q3_write_policy.hpp"
#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::test_support {

inline std::string ReferenceQ3Sql(const std::string &market_segment, const std::string &cutoff_date) {
	return "SELECT l_orderkey, revenue, o_orderdate, o_shippriority FROM ("
	       "SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, o_orderdate, o_shippriority "
	       "FROM customer, orders, lineitem "
	       "WHERE c_mktsegment = '" +
	       market_segment +
	       "' AND c_custkey = o_custkey AND l_orderkey = o_orderkey "
	       "AND o_orderdate < DATE '" +
	       cutoff_date + "' AND l_shipdate > DATE '" + cutoff_date +
	       "' GROUP BY l_orderkey, o_orderdate, o_shippriority "
	       "ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority LIMIT 10)";
}

inline std::string AcceleratorQ3Sql(const std::string &market_segment, const std::string &cutoff_date) {
	return "SELECT * FROM accel_tpch_q3('" + market_segment + "', DATE '" + cutoff_date + "')";
}

inline std::string AcceleratorQ3AuxSql(const std::string &market_segment, const std::string &cutoff_date) {
	return "SELECT * FROM accel_tpch_q3_aux('" + market_segment + "', DATE '" + cutoff_date + "')";
}

inline std::string CanonicalizeQ3OrderSql(const std::string &sql) {
	return "SELECT l_orderkey, revenue, o_orderdate, o_shippriority FROM (" + sql +
	       ") ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority";
}

inline void RequireEqualQ3Results(duckdb::Connection &connection, const std::string &label,
                                  const std::string &reference_sql, const std::string &accelerator_sql) {
	auto expected = connection.Query(CanonicalizeQ3OrderSql(reference_sql));
	auto actual = connection.Query(CanonicalizeQ3OrderSql(accelerator_sql));
	if (expected == nullptr || expected->HasError()) {
		throw std::runtime_error(label + ": reference failed: " + (expected ? expected->GetError() : "null"));
	}
	if (actual == nullptr || actual->HasError()) {
		throw std::runtime_error(label + ": accelerator failed: " + (actual ? actual->GetError() : "null"));
	}
	auto &expected_m = expected->Cast<duckdb::MaterializedQueryResult>();
	auto &actual_m = actual->Cast<duckdb::MaterializedQueryResult>();
	if (expected_m.RowCount() != actual_m.RowCount()) {
		throw std::runtime_error(label + ": row counts differ");
	}
	for (duckdb::idx_t row = 0; row < expected_m.RowCount(); row++) {
		for (duckdb::idx_t col = 0; col < expected_m.ColumnCount(); col++) {
			if (expected_m.GetValue(col, row).ToString() != actual_m.GetValue(col, row).ToString()) {
				throw std::runtime_error(label + ": mismatch at row " + std::to_string(row) + ", col " +
				                         std::to_string(col));
			}
		}
	}
}

inline void RequireAuxSegmentPartitionReadable(duckdb::Connection &connection, const std::string &label,
                                               const std::string &market_segment, bool expect_readable = true) {
	const auto partition = PartitionKeyFromMktsegment(market_segment);
	auto segment = TryGetReadableSegmentPartition(*connection.context, partition);
	if (expect_readable && !segment) {
		throw std::runtime_error(label + ": expected readable segment partition for " + market_segment);
	}
	if (!expect_readable && segment) {
		throw std::runtime_error(label + ": expected invalid segment partition for " + market_segment);
	}
}

inline void ClearQ3MaintenanceOverrides() {
	unsetenv("Q3_WRITE_POLICY");
	unsetenv("Q3_FORCE_ACTION");
	unsetenv("ACCEL_Q3_MAINTENANCE_MODE");
}

inline void EnableHybridMaintenance() {
	ClearQ3MaintenanceOverrides();
}

inline void RequireNoIncrementalFailures(const std::string &label) {
	const auto &counters = GetQ3AuxCounters();
	if (counters.incremental_apply_failures != 0) {
		throw std::runtime_error(label + ": expected zero incremental_apply_failures");
	}
}

} // namespace accelerator_codegen::test_support
