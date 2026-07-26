#pragma once

#include <chrono>
#include <cstdlib>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q14_aux.hpp"
#include "accelerator_codegen/tpch_q14_maintenance.hpp"
#include "accelerator_codegen/tpch_q14_write_policy.hpp"
#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "test_support/q14_aux_helpers.hpp"

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace q14_eval {

using Clock = std::chrono::steady_clock;

inline std::string GetTpchDbPath() {
	const char *env_path = std::getenv("ACCEL_TPCH_DB");
	return (env_path && *env_path) ? env_path : "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
}

inline int GetEnvInt(const char *name, int fallback) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		return fallback;
	}
	return std::stoi(value);
}

inline std::string GetEnvString(const char *name, const std::string &fallback = "") {
	const char *value = std::getenv(name);
	return value ? value : fallback;
}

inline void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

inline void ExecuteAndConsume(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Require(result != nullptr && !result->HasError(), result ? result->GetError() : "null result");
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
		for (duckdb::idx_t col = 0; col < materialized.ColumnCount(); col++) {
			(void)materialized.GetValue(col, row);
		}
	}
}

inline double ElapsedUs(const Clock::time_point &start, const Clock::time_point &end) {
	return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

inline std::string QueryMonthEndExclusive(const std::string &query_start_date) {
	return "DATE '" + query_start_date + "' + INTERVAL '1' month";
}

inline std::string LineitemMonthFilter(const std::string &query_start_date) {
	return "l_shipdate >= DATE '" + query_start_date + "' AND l_shipdate < " + QueryMonthEndExclusive(query_start_date);
}

inline double SumMaintenanceOverheadUs() {
	double total = 0.0;
	for (const auto &record : accelerator_codegen::GetQ14MaintenanceDecisionLog()) {
		total += record.actual_cost_us;
	}
	return total;
}

struct FanoutCatalog {
	std::vector<int64_t> ordered_partkeys;
	int64_t LowPartKey(int seq) const {
		if (ordered_partkeys.empty()) {
			return 1;
		}
		const auto idx = static_cast<size_t>(seq) % std::max<size_t>(1, ordered_partkeys.size() / 3);
		return ordered_partkeys[idx];
	}
	int64_t MediumPartKey(int seq) const {
		if (ordered_partkeys.empty()) {
			return 1;
		}
		const auto third = ordered_partkeys.size() / 3;
		const auto idx = third + (static_cast<size_t>(seq) % std::max<size_t>(1, third));
		return ordered_partkeys[std::min(idx, ordered_partkeys.size() - 1)];
	}
	int64_t HighPartKey(int seq) const {
		if (ordered_partkeys.empty()) {
			return 1;
		}
		const auto third = ordered_partkeys.size() / 3;
		const auto idx = 2 * third + (static_cast<size_t>(seq) % std::max<size_t>(1, ordered_partkeys.size() - 2 * third));
		return ordered_partkeys[std::min(idx, ordered_partkeys.size() - 1)];
	}
};

inline FanoutCatalog LoadFanoutCatalog(duckdb::Connection &connection) {
	FanoutCatalog catalog;
	auto result = connection.Query(
	    "SELECT p.p_partkey FROM part p "
	    "JOIN (SELECT l_partkey, COUNT(*) AS fanout FROM lineitem GROUP BY l_partkey) f "
	    "ON p.p_partkey = f.l_partkey ORDER BY f.fanout, p.p_partkey");
	Require(result != nullptr && !result->HasError(), result->GetError());
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
		catalog.ordered_partkeys.push_back(materialized.GetValue(0, row).GetValue<int64_t>());
	}
	return catalog;
}

inline int64_t CountLineitemsInMonth(duckdb::Connection &connection, const std::string &query_start_date) {
	auto result = connection.Query("SELECT COUNT(*) FROM lineitem WHERE " + LineitemMonthFilter(query_start_date));
	Require(result != nullptr && !result->HasError(), result->GetError());
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	return materialized.GetValue(0, 0).GetValue<int64_t>();
}

inline std::string LineitemRowTargetSubquery(const std::string &query_start_date, int seq, int64_t limit) {
	return "SELECT l_orderkey, l_linenumber FROM lineitem WHERE " + LineitemMonthFilter(query_start_date) +
	       " ORDER BY l_orderkey, l_linenumber LIMIT " + std::to_string(limit) + " OFFSET " +
	       std::to_string((seq * static_cast<int>(limit)) % 256);
}

inline std::string PartKeysSubquery(int seq, int64_t limit) {
	return "SELECT p_partkey FROM part ORDER BY p_partkey LIMIT " + std::to_string(limit) + " OFFSET " +
	       std::to_string((seq * static_cast<int>(limit)) % 256);
}

inline void ApplyLineitemRowWrite(duckdb::Connection &connection, const std::string &query_start_date, int seq,
                                  int64_t row_count) {
	if (row_count < 0) {
		ExecuteAndConsume(connection,
		                  "UPDATE lineitem SET l_extendedprice = l_extendedprice * 1.0000001 WHERE " +
		                      LineitemMonthFilter(query_start_date));
		return;
	}
	const auto target = LineitemRowTargetSubquery(query_start_date, seq, row_count);
	ExecuteAndConsume(connection,
	                  "UPDATE lineitem SET l_extendedprice = l_extendedprice * 1.0001, l_discount = l_discount "
	                  "WHERE (l_orderkey, l_linenumber) IN (" +
	                      target + ")");
}

inline void ApplyPartKeyWrite(duckdb::Connection &connection, int seq, int64_t part_count) {
	const auto target = PartKeysSubquery(seq, part_count);
	ExecuteAndConsume(connection, "UPDATE part SET p_type = CASE WHEN p_type LIKE 'PROMO%' THEN 'LARGE STEEL' "
	                              "ELSE 'PROMO STEEL' END WHERE p_partkey IN (" +
	                                  target + ")");
}

inline void ApplyFanoutBucketWrite(duckdb::Connection &connection, const FanoutCatalog &catalog,
                                   const std::string &bucket, int seq) {
	int64_t partkey = 1;
	if (bucket == "low") {
		partkey = catalog.LowPartKey(seq);
	} else if (bucket == "medium") {
		partkey = catalog.MediumPartKey(seq);
	} else if (bucket == "high") {
		partkey = catalog.HighPartKey(seq);
	} else {
		throw std::runtime_error("unknown fanout bucket: " + bucket);
	}
	ExecuteAndConsume(connection, "UPDATE part SET p_type = CASE WHEN p_type LIKE 'PROMO%' THEN 'LARGE STEEL' "
	                              "ELSE 'PROMO STEEL' END WHERE p_partkey = " +
	                                  std::to_string(partkey));
}

inline std::string MonthStartDate(int month_index) {
	const int month = (month_index % 12) + 1;
	std::ostringstream oss;
	oss << "1995-" << std::setw(2) << std::setfill('0') << month << "-15";
	return oss.str();
}

inline void ApplyPartitionWrite(duckdb::Connection &connection, const std::string &query_start_date, int seq,
                                const std::string &partition_spec) {
	if (partition_spec == "1") {
		const auto target = LineitemRowTargetSubquery(query_start_date, seq, 1);
		ExecuteAndConsume(connection,
		                  "UPDATE lineitem SET l_extendedprice = l_extendedprice * 1.0001 WHERE (l_orderkey, "
		                  "l_linenumber) IN (" +
		                      target + ")");
		return;
	}
	if (partition_spec == "all") {
		ExecuteAndConsume(connection, "UPDATE lineitem SET l_extendedprice = l_extendedprice * 1.0000001");
		return;
	}
	const int partition_count = std::stoi(partition_spec);
	for (int i = 0; i < partition_count; i++) {
		const auto month = MonthStartDate((seq + i) % 12);
		const auto target = LineitemRowTargetSubquery(query_start_date, seq + i, 1);
		ExecuteAndConsume(connection, "UPDATE lineitem SET l_shipdate = DATE '" + month +
		                                  "' WHERE (l_orderkey, l_linenumber) IN (" + target + ")");
	}
}

inline void ApplyCrossoverWrite(duckdb::Connection &connection, const std::string &axis, const std::string &value,
                                const std::string &query_start_date, int seq, const FanoutCatalog &catalog) {
	if (axis == "lineitem_rows") {
		int64_t rows = 0;
		if (value == "full_month") {
			rows = -1;
		} else {
			rows = std::stoll(value);
		}
		ApplyLineitemRowWrite(connection, query_start_date, seq, rows);
		return;
	}
	if (axis == "part_keys") {
		ApplyPartKeyWrite(connection, seq, std::stoll(value));
		return;
	}
	if (axis == "fanout_bucket") {
		ApplyFanoutBucketWrite(connection, catalog, value, seq);
		return;
	}
	if (axis == "partitions") {
		ApplyPartitionWrite(connection, query_start_date, seq, value);
		return;
	}
	if (axis == "queries_per_write") {
		ApplyLineitemRowWrite(connection, query_start_date, seq, 1);
		return;
	}
	throw std::runtime_error("unknown crossover axis: " + axis);
}

inline void ApplyPointLineitemWrite(duckdb::Connection &connection, const std::string &query_start_date, int seq) {
	ApplyLineitemRowWrite(connection, query_start_date, seq, 1);
}

inline void ApplyBulkLineitemMonthWrite(duckdb::Connection &connection, const std::string &query_start_date) {
	ExecuteAndConsume(connection,
	                  "UPDATE lineitem SET l_extendedprice = l_extendedprice * 1.0000001 WHERE " +
	                      LineitemMonthFilter(query_start_date));
}

inline bool ShouldWriteAtQueryIndex(int query_index, int queries_per_write) {
	return queries_per_write > 0 && query_index > 0 && (query_index % queries_per_write) == 0;
}

} // namespace q14_eval
