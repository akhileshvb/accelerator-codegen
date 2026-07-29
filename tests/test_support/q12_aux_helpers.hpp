#pragma once

#include <stdexcept>
#include <string>

#include "accelerator_codegen/tpch_q12_aux.hpp"
#include "accelerator_codegen/tpch_q12_write_policy.hpp"
#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::test_support {

inline std::string AcceleratorQ12Sql(const std::string &shipmode1, const std::string &shipmode2,
                                      const std::string &start_date) {
	return "SELECT * FROM accel_tpch_q12('" + shipmode1 + "', '" + shipmode2 + "', DATE '" + start_date + "')";
}

inline std::string ReferenceQ12Sql(const std::string &shipmode1, const std::string &shipmode2,
                                   const std::string &start_date) {
	return AcceleratorQ12Sql(shipmode1, shipmode2, start_date);
}

inline std::string AcceleratorQ12AuxSql(const std::string &shipmode1, const std::string &shipmode2,
                                        const std::string &start_date) {
	return "SELECT * FROM accel_tpch_q12_aux('" + shipmode1 + "', '" + shipmode2 + "', DATE '" + start_date + "')";
}

inline void RequireEqualQ12Results(duckdb::Connection &connection, const std::string &label,
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

inline void RequireAuxDayPartitionReadable(duckdb::Connection &connection, const std::string &label,
                                         duckdb::date_t receiptdate, bool expect_readable = true) {
	const auto partition = PartitionKeyFromReceiptdate(receiptdate);
	auto day = TryGetReadableQ12DayPartition(*connection.context, partition);
	if (expect_readable && !day) {
		throw std::runtime_error(label + ": expected readable day partition");
	}
	if (!expect_readable && day) {
		throw std::runtime_error(label + ": expected invalid day partition");
	}
}

inline void ClearQ12MaintenanceOverrides() {
	unsetenv("Q12_WRITE_POLICY");
	unsetenv("Q12_FORCE_ACTION");
	unsetenv("ACCEL_Q12_MAINTENANCE_MODE");
}

inline void EnableHybridMaintenance() {
	ClearQ12MaintenanceOverrides();
}

inline void RequireNoIncrementalFailures(const std::string &label) {
	const auto &counters = GetQ12AuxCounters();
	if (counters.incremental_apply_failures != 0) {
		throw std::runtime_error(label + ": expected zero incremental_apply_failures");
	}
}

} // namespace accelerator_codegen::test_support
