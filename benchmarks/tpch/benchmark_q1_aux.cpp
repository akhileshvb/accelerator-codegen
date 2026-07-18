#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q1.hpp"
#include "accelerator_codegen/tpch_q1_aux.hpp"
#include "accelerator_codegen/tpch_q1_fused.hpp"
#include "test_support/q1_manifest.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include "duckdb.hpp"

namespace {

using Clock = std::chrono::steady_clock;

std::string GetTpchDbPath() {
	const char *env_path = std::getenv("ACCEL_TPCH_DB");
	if (env_path && *env_path) {
		return env_path;
	}
	return "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
}

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

std::string ReferenceQ1Sql(int32_t day_offset) {
	return "SELECT l_returnflag, l_linestatus, "
	       "sum(l_quantity) AS sum_qty, "
	       "sum(l_extendedprice) AS sum_base_price, "
	       "sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price, "
	       "sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge, "
	       "avg(l_quantity) AS avg_qty, "
	       "avg(l_extendedprice) AS avg_price, "
	       "avg(l_discount) AS avg_disc, "
	       "count(*) AS count_order "
	       "FROM lineitem "
	       "WHERE l_shipdate <= date '1998-12-01' - INTERVAL (" +
	       std::to_string(day_offset) +
	       ") DAY "
	       "GROUP BY l_returnflag, l_linestatus "
	       "ORDER BY l_returnflag, l_linestatus";
}

std::string AcceleratorQ1Sql(int32_t day_offset) {
	return "SELECT * FROM accel_tpch_q1(" + std::to_string(day_offset) + ")";
}

double Median(std::vector<double> samples) {
	Require(!samples.empty(), "empty samples");
	std::sort(samples.begin(), samples.end());
	const auto n = samples.size();
	if (n % 2 == 1) {
		return samples[n / 2];
	}
	return 0.5 * (samples[n / 2 - 1] + samples[n / 2]);
}

double Mean(const std::vector<double> &samples) {
	return std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());
}

double ElapsedMs(Clock::time_point start, Clock::time_point end) {
	return std::chrono::duration<double, std::milli>(end - start).count();
}

struct Timing {
	double average_ms = 0;
	double median_ms = 0;
};

Timing BenchmarkSql(duckdb::Connection &connection, const std::string &sql, int warmup, int measured) {
	for (int i = 0; i < warmup; i++) {
		auto result = connection.Query(sql);
		Require(result != nullptr && !result->HasError(), result ? result->GetError() : "null result");
	}
	std::vector<double> samples;
	samples.reserve(measured);
	for (int i = 0; i < measured; i++) {
		const auto start = Clock::now();
		auto result = connection.Query(sql);
		const auto end = Clock::now();
		Require(result != nullptr && !result->HasError(), result ? result->GetError() : "null result");
		samples.push_back(ElapsedMs(start, end));
	}
	return {Mean(samples), Median(samples)};
}

} // namespace

int main() {
	try {
		const int warmup = 3;
		const int measured = 10;

		duckdb::DuckDB database(GetTpchDbPath().c_str());
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		auto pragma = connection.Query("PRAGMA threads=1");
		Require(pragma != nullptr && !pragma->HasError(), "failed to set threads=1");

		accelerator_codegen::InvalidateQ1AuxState();

		std::cout << std::fixed << std::setprecision(3);
		std::cout << "=== Q1 aux prefix-state PoC benchmark (SF1, 1 thread) ===\n";

		// Build cost / memory
		{
			connection.BeginTransaction();
			const auto start = Clock::now();
			accelerator_codegen::BuildQ1AuxState(*connection.context);
			const auto build_ms = ElapsedMs(start, Clock::now());
			connection.Commit();
			const auto &diag = accelerator_codegen::GetQ1AuxStateDiagnostics();
			std::cout << "aux_build_ms=" << build_ms << " diag_build_ms=" << (diag.build_ns / 1e6)
			          << " memory_bytes=" << diag.memory_bytes << " date_entries=" << diag.date_entries
			          << " group_entries=" << diag.group_entries << " prefix_cells=" << diag.prefix_cells
			          << " source_rows=" << diag.source_rows << "\n";
		}

		std::cout << "label,duckdb_sql_ms,native_aligned_ms,aux_lookup_ms,aux_e2e_with_rebuild_ms,"
		             "speedup_vs_sql,speedup_vs_native\n";

		double build_ms_for_breakeven = accelerator_codegen::GetQ1AuxStateDiagnostics().build_ns / 1e6;
		double sum_sql = 0;
		double sum_native = 0;
		double sum_aux = 0;

		for (const auto &entry : accelerator_codegen::test_support::kQ1Manifest) {
			const auto sql_t = BenchmarkSql(connection, ReferenceQ1Sql(entry.day_offset), warmup, measured);

			setenv("ACCEL_Q1_FUSED_SCAN", "1", 1);
			setenv("ACCEL_Q1_FUSED_ALIGNED_PAYLOAD", "1", 1);
			setenv("ACCEL_Q1_FUSED_FIXED_SLOT", "1", 1);
			unsetenv("ACCEL_Q1_AUX_STATE");
			const auto native_t = BenchmarkSql(connection, AcceleratorQ1Sql(entry.day_offset), warmup, measured);

			unsetenv("ACCEL_Q1_FUSED_SCAN");
			unsetenv("ACCEL_Q1_FUSED_ALIGNED_PAYLOAD");
			setenv("ACCEL_Q1_AUX_STATE", "1", 1);
			// Lookup-only (state already built).
			const auto aux_t = BenchmarkSql(connection, AcceleratorQ1Sql(entry.day_offset), warmup, measured);

			// End-to-end including rebuild once per measured sample.
			std::vector<double> e2e_samples;
			e2e_samples.reserve(measured);
			for (int i = 0; i < warmup; i++) {
				accelerator_codegen::InvalidateQ1AuxState();
				auto result = connection.Query(AcceleratorQ1Sql(entry.day_offset));
				Require(result != nullptr && !result->HasError(), result ? result->GetError() : "null");
			}
			for (int i = 0; i < measured; i++) {
				accelerator_codegen::InvalidateQ1AuxState();
				const auto start = Clock::now();
				auto result = connection.Query(AcceleratorQ1Sql(entry.day_offset));
				const auto end = Clock::now();
				Require(result != nullptr && !result->HasError(), result ? result->GetError() : "null");
				e2e_samples.push_back(ElapsedMs(start, end));
			}
			const auto e2e_med = Median(e2e_samples);

			// Restore warm state for next seed.
			connection.BeginTransaction();
			accelerator_codegen::BuildQ1AuxState(*connection.context);
			connection.Commit();

			const auto speedup_sql = sql_t.median_ms / aux_t.median_ms;
			const auto speedup_native = native_t.median_ms / aux_t.median_ms;
			std::cout << entry.label << "," << sql_t.median_ms << "," << native_t.median_ms << "," << aux_t.median_ms
			          << "," << e2e_med << "," << speedup_sql << "," << speedup_native << "\n";

			sum_sql += sql_t.median_ms;
			sum_native += native_t.median_ms;
			sum_aux += aux_t.median_ms;
			build_ms_for_breakeven = accelerator_codegen::GetQ1AuxStateDiagnostics().build_ns / 1e6;
		}

		const auto n = static_cast<double>(accelerator_codegen::test_support::kQ1Manifest.size());
		const auto avg_sql = sum_sql / n;
		const auto avg_native = sum_native / n;
		const auto avg_aux = sum_aux / n;
		const auto saved_vs_sql = avg_sql - avg_aux;
		const auto saved_vs_native = avg_native - avg_aux;
		std::cout << "averages_ms sql=" << avg_sql << " native_aligned=" << avg_native << " aux_lookup=" << avg_aux
		          << "\n";
		std::cout << "break_even_queries_vs_sql="
		          << (saved_vs_sql > 0 ? (build_ms_for_breakeven / saved_vs_sql) : -1.0) << "\n";
		std::cout << "break_even_queries_vs_native="
		          << (saved_vs_native > 0 ? (build_ms_for_breakeven / saved_vs_native) : -1.0) << "\n";
		std::cout << "assumptions=immutable_snapshot;rejects_local_writes;row_count_stale_check;"
		             "no_incremental_maintenance;UPDATE_preserving_rowcount_undetected\n";

		unsetenv("ACCEL_Q1_AUX_STATE");
		unsetenv("ACCEL_Q1_FUSED_SCAN");
		unsetenv("ACCEL_Q1_FUSED_ALIGNED_PAYLOAD");
		unsetenv("ACCEL_Q1_FUSED_FIXED_SLOT");
		accelerator_codegen::InvalidateQ1AuxState();
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_q1_aux failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
