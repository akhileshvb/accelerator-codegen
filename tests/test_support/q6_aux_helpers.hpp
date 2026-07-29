#pragma once

#include <stdexcept>
#include <string>

#include "accelerator_codegen/tpch_q6_aux.hpp"
#include "accelerator_codegen/tpch_q6_write_policy.hpp"
#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::test_support {

inline std::string ReferenceQ6Sql(const std::string &start_date, const std::string &discount, const std::string &quantity) {
	return "SELECT revenue FROM accel_tpch_q6(DATE '" + start_date + "', " + discount + ", " + quantity + ")";
}

inline std::string AcceleratorQ6AuxSql(const std::string &start_date, const std::string &discount, const std::string &quantity) {
	return "SELECT revenue FROM accel_tpch_q6_aux(DATE '" + start_date + "', " + discount + ", " + quantity + ")";
}

inline void RequireEqualQ6Revenue(duckdb::Connection &connection, const std::string &label,
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
		if (expected_m.GetValue(0, row).ToString() != actual_m.GetValue(0, row).ToString()) {
			throw std::runtime_error(label + ": revenue mismatch at row " + std::to_string(row));
		}
	}
}

inline void RequireAuxDayPartitionReadable(duckdb::Connection &connection, const std::string &label,
                                           duckdb::date_t shipdate, bool expect_readable = true) {
	const auto partition = Q6PartitionKeyFromShipdate(shipdate);
	auto day = TryGetReadableQ6DayPartition(*connection.context, partition);
	if (expect_readable && !day) {
		throw std::runtime_error(label + ": expected readable day partition");
	}
	if (!expect_readable && day) {
		throw std::runtime_error(label + ": expected invalid day partition");
	}
}

inline void ClearQ6MaintenanceOverrides() {
	unsetenv("Q6_WRITE_POLICY");
	unsetenv("Q6_FORCE_ACTION");
	unsetenv("ACCEL_Q6_MAINTENANCE_MODE");
}

inline void EnableHybridMaintenance() {
	ClearQ6MaintenanceOverrides();
}

inline void RequireNoIncrementalFailures(const std::string &label) {
	const auto &counters = GetQ6AuxCounters();
	if (counters.incremental_apply_failures != 0) {
		throw std::runtime_error(label + ": expected zero incremental_apply_failures");
	}
}

} // namespace accelerator_codegen::test_support
