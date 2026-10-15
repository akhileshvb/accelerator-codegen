#include "eval/tests/test_support/db_paths.hpp"
#include "backends/duckdb/engine/duckdb_scan_adapter.hpp"
#include "backends/duckdb/api/register.hpp"
#include "tpch/q4/duckdb/base/query.hpp"
#include "tpch/q4/tests/q4_manifest.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {
using Clock = std::chrono::steady_clock;

std::string GetTpchDbPath() {
	return accelerator_codegen::test_support::TpchDbPath();
}

int GetEnvInt(const char *name, int fallback) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		return fallback;
	}
	return std::stoi(value);
}

std::string GetEnvString(const char *name) {
	const char *value = std::getenv(name);
	return value ? value : "";
}

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

std::string ReadFile(const std::string &path) {
	std::ifstream input(path);
	Require(input.good(), "failed to open file: " + path);
	std::ostringstream buffer;
	buffer << input.rdbuf();
	return buffer.str();
}

std::vector<std::string> SplitCommaSeparated(const std::string &input) {
	std::vector<std::string> result;
	std::stringstream stream(input);
	std::string item;
	while (std::getline(stream, item, ',')) {
		if (!item.empty()) {
			result.push_back(item);
		}
	}
	return result;
}

bool MatchesFilter(const std::string &label, const std::vector<std::string> &filters) {
	if (filters.empty()) {
		return true;
	}
	return std::find(filters.begin(), filters.end(), label) != filters.end();
}

std::string AcceleratorQ4Sql(const accelerator_codegen::test_support::Q4ManifestEntry &entry) {
	return std::string("SELECT * FROM accel_tpch_q4(DATE '") + entry.start_date + "')";
}

void ExecuteAndConsume(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Require(result != nullptr, "query returned null result");
	Require(!result->HasError(), result->GetError());
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	std::size_t bytes_seen = 0;
	for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
		for (duckdb::idx_t col = 0; col < materialized.ColumnCount(); col++) {
			bytes_seen += materialized.GetValue(col, row).ToString().size();
		}
	}
	Require(bytes_seen != static_cast<std::size_t>(-1), "unreachable bytes_seen guard");
}

struct TimingStats {
	double mean_us = 0.0;
	double median_us = 0.0;
};

template <class Callback>
TimingStats BenchmarkOperation(int warmup_iterations, int measured_iterations, Callback &&callback) {
	for (int i = 0; i < warmup_iterations; i++) {
		callback();
	}

	std::vector<double> samples_us;
	samples_us.reserve(measured_iterations);
	for (int i = 0; i < measured_iterations; i++) {
		const auto start = Clock::now();
		callback();
		const auto end = Clock::now();
		samples_us.push_back(
		    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()));
	}

	TimingStats stats;
	for (const auto sample : samples_us) {
		stats.mean_us += sample;
	}
	stats.mean_us /= static_cast<double>(samples_us.size());

	std::sort(samples_us.begin(), samples_us.end());
	const auto middle = samples_us.size() / 2;
	stats.median_us = samples_us.size() % 2 == 0 ? (samples_us[middle - 1] + samples_us[middle]) / 2.0
	                                              : samples_us[middle];
	return stats;
}

TimingStats BenchmarkQuery(duckdb::Connection &connection, const std::string &sql, int warmup_iterations,
                           int measured_iterations) {
	return BenchmarkOperation(warmup_iterations, measured_iterations, [&]() { ExecuteAndConsume(connection, sql); });
}

template <class Callback>
void WithTransaction(duckdb::Connection &connection, Callback &&callback) {
	const bool started_transaction = !connection.HasActiveTransaction();
	if (started_transaction) {
		connection.BeginTransaction();
	}
	try {
		callback(*connection.context);
		if (started_transaction) {
			connection.Commit();
		}
	} catch (...) {
		if (started_transaction && connection.HasActiveTransaction()) {
			connection.Rollback();
		}
		throw;
	}
}

struct Q4PhaseProfile {
	accelerator_codegen::Q4ProfileCounters counters;
	double lineitem_scan_us = 0.0;
	double lineitem_set_build_us = 0.0;
	double orders_scan_us = 0.0;
	double orders_aggregate_us = 0.0;
	double finalize_output_us = 0.0;
};

Q4PhaseProfile ProfileQ4Execution(duckdb::Connection &connection, const accelerator_codegen::Q4Parameters &parameters) {
	Q4PhaseProfile profile;
	WithTransaction(connection, [&](duckdb::ClientContext &context) {
		auto lineitem_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ4LineitemScanSpec(parameters));
		auto orders_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ4OrdersScanSpec(parameters));
		auto kernel_instance = accelerator_codegen::CreateQ4ProfilingKernel(parameters, profile.counters);
		auto &kernel = dynamic_cast<accelerator_codegen::Q4Kernel &>(*kernel_instance);

		if (accelerator_codegen::Q4LineitemFusedScanEnabled()) {
			const auto fused_start = Clock::now();
			kernel.RunFusedLineitemScan(context, *lineitem_scan);
			const auto fused_end = Clock::now();
			const auto fused_us =
			    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(fused_end - fused_start).count());
			// Attribute fused wall time using decode vs predicate/insert nanosecond split when available.
			const auto decode_us = static_cast<double>(profile.counters.fused_decode_ns) / 1000.0;
			const auto work_us = static_cast<double>(profile.counters.fused_predicate_insert_ns) / 1000.0;
			const auto accounted = decode_us + work_us;
			if (accounted > 0.0) {
				profile.lineitem_scan_us = fused_us * (decode_us / accounted);
				profile.lineitem_set_build_us = fused_us * (work_us / accounted);
			} else {
				profile.lineitem_scan_us = fused_us;
			}
		} else {
			auto lineitem_source = accelerator_codegen::CreateNativeChunkSource(context, *lineitem_scan);
			while (true) {
				const auto scan_start = Clock::now();
				const bool has_chunk = lineitem_source->Next();
				const auto scan_end = Clock::now();
				profile.lineitem_scan_us += static_cast<double>(
				    std::chrono::duration_cast<std::chrono::microseconds>(scan_end - scan_start).count());
				if (!has_chunk) {
					break;
				}
				const auto build_start = Clock::now();
				kernel.ConsumeInput(static_cast<duckdb::idx_t>(accelerator_codegen::Q4InputId::LINEITEM),
				                    lineitem_source->Chunk());
				const auto build_end = Clock::now();
				profile.lineitem_set_build_us += static_cast<double>(
				    std::chrono::duration_cast<std::chrono::microseconds>(build_end - build_start).count());
			}
		}

		auto orders_source = accelerator_codegen::CreateNativeChunkSource(context, *orders_scan);
		while (true) {
			const auto scan_start = Clock::now();
			const bool has_chunk = orders_source->Next();
			const auto scan_end = Clock::now();
			profile.orders_scan_us +=
			    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(scan_end - scan_start).count());
			if (!has_chunk) {
				break;
			}
			const auto agg_start = Clock::now();
			kernel.ConsumeInput(static_cast<duckdb::idx_t>(accelerator_codegen::Q4InputId::ORDERS),
			                    orders_source->Chunk());
			const auto agg_end = Clock::now();
			profile.orders_aggregate_us +=
			    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(agg_end - agg_start).count());
		}

		const auto finalize_start = Clock::now();
		kernel.Finalize();
		duckdb::DataChunk output;
		output.Initialize(context, {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT});
		while (kernel.ProduceOutput(output)) {
			output.Reset();
		}
		const auto finalize_end = Clock::now();
		profile.finalize_output_us += static_cast<double>(
		    std::chrono::duration_cast<std::chrono::microseconds>(finalize_end - finalize_start).count());
	});
	return profile;
}

} // namespace

int main() {
	try {
		const int warmup_iterations = GetEnvInt("ACCEL_BENCH_WARMUP", 3);
		const int measured_iterations = GetEnvInt("ACCEL_BENCH_ITERS", 10);
		const auto filters = SplitCommaSeparated(GetEnvString("ACCEL_BENCH_FILTER"));

		duckdb::DuckDB database(GetTpchDbPath().c_str());
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		auto pragma_result = connection.Query("PRAGMA threads=1");
		Require(pragma_result != nullptr && !pragma_result->HasError(), "failed to set PRAGMA threads=1");

		std::cout
		    << "label,reference_mean_us,reference_median_us,accelerator_mean_us,accelerator_median_us,speedup_mean,"
		       "lineitem_scan_us,lineitem_set_build_us,orders_scan_us,orders_aggregate_us,finalize_output_us,"
		       "scanned_lineitems,qualifying_lineitems,unique_orderkeys,dup_qualifying,date_survivors,set_probes,"
		       "set_hits,set_misses,priority_groups,set_memory_bytes,set_capacity_bits,set_resizes,load_factor,"
		       "line_flat,line_fallback,order_flat,order_dict,order_fallback,"
		       "prio_fixed,prio_generic,o_priority_dict_nullable,fused_lineitem,fused_flat_id,fused_nested,fused_scalar\n";

		int rows = 0;
		double total_reference_mean_us = 0.0;
		double total_accelerator_mean_us = 0.0;
		double total_accelerator_median_us = 0.0;

		for (const auto &entry : accelerator_codegen::test_support::kQ4Manifest) {
			if (!MatchesFilter(entry.label, filters)) {
				continue;
			}

			const auto reference_sql = ReadFile(entry.sql_path);
			const auto accelerator_sql = AcceleratorQ4Sql(entry);
			const auto reference_stats =
			    BenchmarkQuery(connection, reference_sql, warmup_iterations, measured_iterations);
			const auto accelerator_stats =
			    BenchmarkQuery(connection, accelerator_sql, warmup_iterations, measured_iterations);

			auto parameters = accelerator_codegen::MakeQ4Parameters(duckdb::Date::FromString(entry.start_date));
			const auto profile = ProfileQ4Execution(connection, parameters);
			const auto speedup_mean = reference_stats.mean_us / accelerator_stats.mean_us;

			rows += 1;
			total_reference_mean_us += reference_stats.mean_us;
			total_accelerator_mean_us += accelerator_stats.mean_us;
			total_accelerator_median_us += accelerator_stats.median_us;

			std::cout << entry.label << ',' << std::fixed << std::setprecision(2) << reference_stats.mean_us << ','
			          << reference_stats.median_us << ',' << accelerator_stats.mean_us << ','
			          << accelerator_stats.median_us << ',' << speedup_mean << ',' << profile.lineitem_scan_us << ','
			          << profile.lineitem_set_build_us << ',' << profile.orders_scan_us << ','
			          << profile.orders_aggregate_us << ',' << profile.finalize_output_us << ','
			          << profile.counters.scanned_lineitems << ',' << profile.counters.qualifying_lineitems << ','
			          << profile.counters.unique_qualifying_orderkeys << ','
			          << profile.counters.duplicate_qualifying_lineitems << ',' << profile.counters.date_filter_survivors
			          << ',' << profile.counters.set_probes << ',' << profile.counters.set_hits << ','
			          << profile.counters.set_misses << ',' << profile.counters.priority_groups << ','
			          << profile.counters.set_memory_bytes << ',' << profile.counters.set_capacity_bits << ','
			          << profile.counters.set_rehash_or_resize_count << ',' << profile.counters.load_factor << ','
			          << profile.counters.lineitem.flat_fast_path_chunks << ','
			          << profile.counters.lineitem.generic_fallback_chunks << ','
			          << profile.counters.orders.flat_fast_path_chunks << ','
			          << profile.counters.orders.dict_fast_path_chunks << ','
			          << profile.counters.orders.generic_fallback_chunks << ','
			          << profile.counters.fixed_priority_hits << ',' << profile.counters.generic_priority_hits << ','
			          << profile.counters.layout_o_orderpriority.dict_flat_nullable << ','
			          << (profile.counters.used_fused_lineitem ? 1 : 0) << ','
			          << profile.counters.fused_flat_identity_vectors << ','
			          << profile.counters.fused_nested_sel_vectors << ','
			          << profile.counters.fused_scalar_fallback_vectors << "\n";
		}

		Require(rows > 0, "benchmark filter selected no Q4 instances");
		const auto average_reference_mean_us = total_reference_mean_us / static_cast<double>(rows);
		const auto average_accelerator_mean_us = total_accelerator_mean_us / static_cast<double>(rows);
		const auto average_accelerator_median_us = total_accelerator_median_us / static_cast<double>(rows);
		const auto average_speedup_mean = average_reference_mean_us / average_accelerator_mean_us;
		std::cout << "summary_avg," << std::fixed << std::setprecision(2) << average_reference_mean_us << ",,"
		          << average_accelerator_mean_us << "," << average_accelerator_median_us << "," << average_speedup_mean
		          << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_q4 failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
