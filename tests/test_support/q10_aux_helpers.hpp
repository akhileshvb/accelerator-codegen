#pragma once

#include <stdexcept>
#include <string>

#include "accelerator_codegen/tpch_q10_aux.hpp"
#include "accelerator_codegen/tpch_q10_write_policy.hpp"
#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::test_support {

inline std::string AcceleratorQ10Sql(const std::string &start_date) {
	return "SELECT * FROM accel_tpch_q10(DATE '" + start_date + "')";
}

inline std::string ReferenceQ10Sql(const std::string &start_date) {
	return AcceleratorQ10Sql(start_date);
}

inline std::string AcceleratorQ10AuxSql(const std::string &start_date) {
	return "SELECT * FROM accel_tpch_q10_aux(DATE '" + start_date + "')";
}

inline void RequireEqualQ10Results(duckdb::Connection &connection, const std::string &label,
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

inline void RequireAuxMonthPartitionReadable(duckdb::Connection &connection, const std::string &label,
                                               duckdb::date_t orderdate, bool expect_readable = true) {
	const auto partition = PartitionKeyFromOrderdate(orderdate);
	auto month = TryGetReadableQ10MonthPartition(*connection.context, partition);
	if (expect_readable && !month) {
		throw std::runtime_error(label + ": expected readable month partition");
	}
	if (!expect_readable && month) {
		throw std::runtime_error(label + ": expected invalid month partition");
	}
}

inline void ClearQ10MaintenanceOverrides() {
	unsetenv("Q10_WRITE_POLICY");
	unsetenv("Q10_FORCE_ACTION");
	unsetenv("ACCEL_Q10_MAINTENANCE_MODE");
}

inline void EnableHybridMaintenance() {
	ClearQ10MaintenanceOverrides();
}

inline void RequireNoIncrementalFailures(const std::string &label) {
	const auto &counters = GetQ10AuxCounters();
	if (counters.incremental_apply_failures != 0) {
		throw std::runtime_error(label + ": expected zero incremental_apply_failures");
	}
}

} // namespace accelerator_codegen::test_support
