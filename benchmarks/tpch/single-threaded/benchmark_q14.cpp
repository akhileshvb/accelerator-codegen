#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "tpch/q14/duckdb/base/query.hpp"
#include "test_support/q14_manifest.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
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
	const char *env_path = std::getenv("ACCEL_TPCH_DB");
	if (env_path && *env_path) {
		return env_path;
	}
	return "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
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

std::string AcceleratorQ14Sql(const accelerator_codegen::test_support::Q14ManifestEntry &entry) {
	return std::string("SELECT * FROM accel_tpch_q14(DATE '") + entry.start_date + "')";
}

bool DoublesMatch(double expected, double actual) {
	if (std::isnan(expected) && std::isnan(actual)) {
		return std::signbit(expected) == std::signbit(actual);
	}
	if (std::isinf(expected) && std::isinf(actual)) {
		return std::signbit(expected) == std::signbit(actual);
	}
	uint64_t expected_bits = 0;
	uint64_t actual_bits = 0;
	std::memcpy(&expected_bits, &expected, sizeof(double));
	std::memcpy(&actual_bits, &actual, sizeof(double));
	return expected_bits == actual_bits;
}

void RequireEqualResults(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
                         const std::string &accelerator_sql) {
	auto expected = connection.Query(reference_sql);
	auto actual = connection.Query(accelerator_sql);
	Require(expected != nullptr && !expected->HasError(), label + " reference failed: " + expected->GetError());
	Require(actual != nullptr && !actual->HasError(), label + " accelerator failed: " + actual->GetError());
	auto &expected_m = expected->Cast<duckdb::MaterializedQueryResult>();
	auto &actual_m = actual->Cast<duckdb::MaterializedQueryResult>();
	Require(expected_m.RowCount() == 1 && actual_m.RowCount() == 1, label + ": row count");
	const auto ev = expected_m.GetValue(0, 0);
	const auto av = actual_m.GetValue(0, 0);
	Require(ev.IsNull() == av.IsNull(), label + ": nullability");
	if (!ev.IsNull()) {
		Require(DoublesMatch(ev.GetValue<double>(), av.GetValue<double>()), label + ": value mismatch");
	}
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
	Require(bytes_seen != static_cast<std::size_t>(-1), "unreachable");
}

struct TimingStats {
	double mean_us = 0.0;
	double median_us = 0.0;
	double mad_us = 0.0;
};

TimingStats SummarizeSamples(std::vector<double> samples_us) {
	TimingStats stats;
	Require(!samples_us.empty(), "no timing samples");
	for (const auto sample : samples_us) {
		stats.mean_us += sample;
	}
	stats.mean_us /= static_cast<double>(samples_us.size());
	std::sort(samples_us.begin(), samples_us.end());
	const auto middle = samples_us.size() / 2;
	stats.median_us = samples_us.size() % 2 == 0 ? (samples_us[middle - 1] + samples_us[middle]) / 2.0
	                                              : samples_us[middle];
	std::vector<double> abs_dev;
	abs_dev.reserve(samples_us.size());
	for (const auto sample : samples_us) {
		abs_dev.push_back(std::abs(sample - stats.median_us));
	}
	std::sort(abs_dev.begin(), abs_dev.end());
	stats.mad_us = abs_dev.size() % 2 == 0 ? (abs_dev[middle - 1] + abs_dev[middle]) / 2.0 : abs_dev[middle];
	return stats;
}

struct DualTiming {
	TimingStats reference;
	TimingStats accelerator;
};

DualTiming BenchmarkPair(duckdb::Connection &connection, const std::string &reference_sql,
                         const std::string &accelerator_sql, int warmup_iterations, int measured_iterations) {
	for (int i = 0; i < warmup_iterations; i++) {
		if ((i % 2) == 0) {
			ExecuteAndConsume(connection, reference_sql);
			ExecuteAndConsume(connection, accelerator_sql);
		} else {
			ExecuteAndConsume(connection, accelerator_sql);
			ExecuteAndConsume(connection, reference_sql);
		}
	}

	std::vector<double> reference_samples;
	std::vector<double> accelerator_samples;
	reference_samples.reserve(measured_iterations);
	accelerator_samples.reserve(measured_iterations);
	for (int i = 0; i < measured_iterations; i++) {
		if ((i % 2) == 0) {
			const auto ref_start = Clock::now();
			ExecuteAndConsume(connection, reference_sql);
			const auto ref_end = Clock::now();
			reference_samples.push_back(
			    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(ref_end - ref_start).count()));
			const auto accel_start = Clock::now();
			ExecuteAndConsume(connection, accelerator_sql);
			const auto accel_end = Clock::now();
			accelerator_samples.push_back(static_cast<double>(
			    std::chrono::duration_cast<std::chrono::microseconds>(accel_end - accel_start).count()));
		} else {
			const auto accel_start = Clock::now();
			ExecuteAndConsume(connection, accelerator_sql);
			const auto accel_end = Clock::now();
			accelerator_samples.push_back(static_cast<double>(
			    std::chrono::duration_cast<std::chrono::microseconds>(accel_end - accel_start).count()));
			const auto ref_start = Clock::now();
			ExecuteAndConsume(connection, reference_sql);
			const auto ref_end = Clock::now();
			reference_samples.push_back(
			    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(ref_end - ref_start).count()));
		}
	}
	DualTiming timing;
	timing.reference = SummarizeSamples(std::move(reference_samples));
	timing.accelerator = SummarizeSamples(std::move(accelerator_samples));
	return timing;
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

struct Q14PhaseProfile {
	accelerator_codegen::Q14ProfileCounters counters;
	double part_scan_us = 0.0;
	double part_consume_us = 0.0;
	double lineitem_scan_us = 0.0;
	double lineitem_consume_us = 0.0;
	double finalize_us = 0.0;
};

Q14PhaseProfile ProfileQ14Execution(duckdb::Connection &connection,
                                    const accelerator_codegen::Q14Parameters &parameters) {
	Q14PhaseProfile profile;
	WithTransaction(connection, [&](duckdb::ClientContext &context) {
		auto part_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ14PartScanSpec(parameters));
		auto lineitem_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ14LineitemScanSpec(parameters));
		auto kernel = accelerator_codegen::CreateQ14ProfilingKernel(parameters, profile.counters);

		auto part_source = accelerator_codegen::CreateNativeChunkSource(context, *part_scan);
		while (true) {
			const auto scan_start = Clock::now();
			const bool has_chunk = part_source->Next();
			const auto scan_end = Clock::now();
			profile.part_scan_us +=
			    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(scan_end - scan_start).count());
			if (!has_chunk) {
				break;
			}
			const auto consume_start = Clock::now();
			kernel->ConsumeInput(static_cast<duckdb::idx_t>(accelerator_codegen::Q14InputId::PART),
			                     part_source->Chunk());
			const auto consume_end = Clock::now();
			profile.part_consume_us += static_cast<double>(
			    std::chrono::duration_cast<std::chrono::microseconds>(consume_end - consume_start).count());
		}

		auto lineitem_source = accelerator_codegen::CreateNativeChunkSource(context, *lineitem_scan);
		while (true) {
			const auto scan_start = Clock::now();
			const bool has_chunk = lineitem_source->Next();
			const auto scan_end = Clock::now();
			profile.lineitem_scan_us +=
			    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(scan_end - scan_start).count());
			if (!has_chunk) {
				break;
			}
			const auto consume_start = Clock::now();
			kernel->ConsumeInput(static_cast<duckdb::idx_t>(accelerator_codegen::Q14InputId::LINEITEM),
			                     lineitem_source->Chunk());
			const auto consume_end = Clock::now();
			profile.lineitem_consume_us += static_cast<double>(
			    std::chrono::duration_cast<std::chrono::microseconds>(consume_end - consume_start).count());
		}

		const auto finalize_start = Clock::now();
		kernel->Finalize();
		duckdb::DataChunk output;
		output.Initialize(context, {duckdb::LogicalType::DOUBLE});
		while (kernel->ProduceOutput(output)) {
			output.Reset();
		}
		const auto finalize_end = Clock::now();
		profile.finalize_us += static_cast<double>(
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

		std::cout << "label,reference_mean_us,reference_median_us,reference_mad_us,"
		             "accelerator_mean_us,accelerator_median_us,accelerator_mad_us,speedup_median,"
		             "part_scan_us,part_consume_us,lineitem_scan_us,lineitem_consume_us,finalize_us,"
		             "part_init_us,part_build_us,part_dict_chunks,lineitem_dict_chunks,lineitem_shared_sel,"
		             "part_rows,map_cardinality,map_memory_bytes,dense_part,promo_parts,non_promo_parts,"
		             "lineitem_rows,date_survivors,residual_checks,residual_skips,join_hits,join_misses,promo_survivors,"
		             "part_flat,part_dict,part_fallback,lineitem_flat,lineitem_dict,lineitem_fallback,"
		             "p_type_dict_valid,p_type_dict_nullable,l_partkey_dict\n";

		int rows = 0;
		double total_reference_median_us = 0.0;
		double total_accelerator_median_us = 0.0;

		for (const auto &entry : accelerator_codegen::test_support::kQ14Manifest) {
			if (!MatchesFilter(entry.label, filters)) {
				continue;
			}

			const auto reference_sql = ReadFile(entry.sql_path);
			const auto accelerator_sql = AcceleratorQ14Sql(entry);
			RequireEqualResults(connection, entry.label, reference_sql, accelerator_sql);

			const auto timing =
			    BenchmarkPair(connection, reference_sql, accelerator_sql, warmup_iterations, measured_iterations);
			auto parameters =
			    accelerator_codegen::MakeQ14Parameters(duckdb::Date::FromString(entry.start_date));
			const auto profile = ProfileQ14Execution(connection, parameters);
			const auto speedup_median = timing.reference.median_us / timing.accelerator.median_us;

			rows += 1;
			total_reference_median_us += timing.reference.median_us;
			total_accelerator_median_us += timing.accelerator.median_us;

			std::cout << entry.label << ',' << std::fixed << std::setprecision(2) << timing.reference.mean_us << ','
			          << timing.reference.median_us << ',' << timing.reference.mad_us << ','
			          << timing.accelerator.mean_us << ',' << timing.accelerator.median_us << ','
			          << timing.accelerator.mad_us << ',' << speedup_median << ',' << profile.part_scan_us << ','
			          << profile.part_consume_us << ',' << profile.lineitem_scan_us << ','
			          << profile.lineitem_consume_us << ',' << profile.finalize_us << ','
			          << (static_cast<double>(profile.counters.part_state_init_ns) / 1000.0) << ','
			          << (static_cast<double>(profile.counters.part_total_build_ns) / 1000.0) << ','
			          << profile.counters.part.dict_fast_path_chunks << ','
			          << profile.counters.lineitem.dict_fast_path_chunks << ','
			          << profile.counters.lineitem_shared_sel_chunks << ',' << profile.counters.part_rows_scanned << ','
			          << profile.counters.part_map_cardinality << ',' << profile.counters.part_map_memory_bytes << ','
			          << (profile.counters.used_dense_part ? 1 : 0) << ',' << profile.counters.part_promo_count << ','
			          << profile.counters.part_non_promo_count << ',' << profile.counters.lineitem_rows_scanned << ','
			          << profile.counters.date_survivors << ',' << profile.counters.residual_date_checks << ','
			          << profile.counters.residual_date_skips << ',' << profile.counters.join_hits << ','
			          << profile.counters.join_misses << ',' << profile.counters.promo_survivors << ','
			          << profile.counters.part.flat_fast_path_chunks << ','
			          << profile.counters.part.dict_fast_path_chunks << ','
			          << profile.counters.part.generic_fallback_chunks << ','
			          << profile.counters.lineitem.flat_fast_path_chunks << ','
			          << profile.counters.lineitem.dict_fast_path_chunks << ','
			          << profile.counters.lineitem.generic_fallback_chunks << ','
			          << profile.counters.layout_p_type.dict_flat_all_valid << ','
			          << profile.counters.layout_p_type.dict_flat_nullable << ','
			          << profile.counters.layout_l_partkey.dict_flat_all_valid << "\n";
		}

		Require(rows > 0, "benchmark filter selected no Q14 instances");
		const auto average_reference_median_us = total_reference_median_us / static_cast<double>(rows);
		const auto average_accelerator_median_us = total_accelerator_median_us / static_cast<double>(rows);
		const auto average_speedup_median = average_reference_median_us / average_accelerator_median_us;
		std::cout << "summary_avg_median," << std::fixed << std::setprecision(2) << average_reference_median_us
		          << ",,," << average_accelerator_median_us << ",,," << average_speedup_median << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_q14 failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
