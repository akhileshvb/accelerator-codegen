#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "tpch/q3/base/query.hpp"
#include "test_support/q3_manifest.hpp"

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

std::string AcceleratorQ3Sql(const accelerator_codegen::test_support::Q3ManifestEntry &entry) {
	return std::string("SELECT * FROM accel_tpch_q3('") + entry.market_segment + "', DATE '" + entry.cutoff_date + "')";
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

struct PhaseProfile {
	accelerator_codegen::Q3ProfileCounters counters;
	double customer_scan_us = 0.0;
	double orders_scan_us = 0.0;
	double orders_next_us = 0.0;
	double orders_consume_us = 0.0;
	double lineitem_scan_us = 0.0;
	double lineitem_next_us = 0.0;
	double lineitem_consume_us = 0.0;
	double finalize_us = 0.0;
	double total_us = 0.0;
};

PhaseProfile ProfileQ3Execution(duckdb::Connection &connection, const accelerator_codegen::Q3Parameters &parameters) {
	PhaseProfile profile;
	WithTransaction(connection, [&](duckdb::ClientContext &context) {
		const auto total_start = Clock::now();
		auto customer_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ3CustomerScanSpec(parameters));
		auto orders_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ3OrdersScanSpec(parameters));
		auto lineitem_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ3LineitemScanSpec(parameters));

		auto kernel = accelerator_codegen::CreateQ3ProfilingKernel(parameters, profile.counters);

		auto customer_start = Clock::now();
		auto customer_source = accelerator_codegen::CreateNativeChunkSource(context, *customer_scan);
		while (customer_source->Next()) {
			kernel->ConsumeInput(static_cast<duckdb::idx_t>(accelerator_codegen::Q3InputId::CUSTOMER),
			                     customer_source->Chunk());
		}
		profile.customer_scan_us =
		    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - customer_start).count());

		auto orders_start = Clock::now();
		auto orders_source = accelerator_codegen::CreateNativeChunkSource(context, *orders_scan);
		while (true) {
			const auto next_start = Clock::now();
			const bool has_chunk = orders_source->Next();
			profile.orders_next_us += static_cast<double>(
			    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - next_start).count());
			if (!has_chunk) {
				break;
			}
			const auto consume_start = Clock::now();
			kernel->ConsumeInput(static_cast<duckdb::idx_t>(accelerator_codegen::Q3InputId::ORDERS),
			                     orders_source->Chunk());
			profile.orders_consume_us += static_cast<double>(
			    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - consume_start).count());
		}
		profile.orders_scan_us =
		    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - orders_start).count());

		auto lineitem_start = Clock::now();
		auto lineitem_source = accelerator_codegen::CreateNativeChunkSource(context, *lineitem_scan);
		while (true) {
			const auto next_start = Clock::now();
			const bool has_chunk = lineitem_source->Next();
			profile.lineitem_next_us += static_cast<double>(
			    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - next_start).count());
			if (!has_chunk) {
				break;
			}
			const auto consume_start = Clock::now();
			kernel->ConsumeInput(static_cast<duckdb::idx_t>(accelerator_codegen::Q3InputId::LINEITEM),
			                     lineitem_source->Chunk());
			profile.lineitem_consume_us += static_cast<double>(
			    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - consume_start).count());
		}
		profile.lineitem_scan_us = static_cast<double>(
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - lineitem_start).count());

		auto finalize_start = Clock::now();
		kernel->Finalize();
		duckdb::DataChunk output;
		output.Initialize(context, {duckdb::LogicalType::BIGINT, duckdb::LogicalType::DECIMAL(38, 4),
		                            duckdb::LogicalType::DATE, duckdb::LogicalType::INTEGER});
		while (kernel->ProduceOutput(output)) {
			output.Reset();
		}
		profile.finalize_us =
		    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - finalize_start).count());
		profile.total_us =
		    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - total_start).count());
	});
	return profile;
}

void PrintLayout(const char *name, const accelerator_codegen::Q3LayoutHistogram &h) {
	std::cerr << "  " << name << ": flat=" << h.flat_all_valid << " dict_flat=" << h.dict_flat_all_valid
	          << " dict_null=" << h.dict_flat_nullable << " const=" << h.constant_valid << " other=" << h.other << "\n";
}

} // namespace

int main() {
	try {
		const int warmup_iterations = GetEnvInt("ACCEL_BENCH_WARMUP", 3);
		const int measured_iterations = GetEnvInt("ACCEL_BENCH_ITERS", 10);
		const auto filters = SplitCommaSeparated(GetEnvString("ACCEL_BENCH_FILTER"));
		const bool verbose = GetEnvInt("ACCEL_BENCH_VERBOSE", 0) != 0;

		duckdb::DuckDB database(GetTpchDbPath().c_str());
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		auto pragma_result = connection.Query("PRAGMA threads=1");
		Require(pragma_result != nullptr && !pragma_result->HasError(), "failed to set PRAGMA threads=1");

		std::cout << "label,reference_mean_us,reference_median_us,accelerator_mean_us,accelerator_median_us,speedup_mean,"
		             "speedup_median,cust_us,orders_us,line_us,finalize_us,cust_state_bytes,orders_state_bytes,"
		             "qual_customers,qual_orders,qual_lineitems,touched,top_rows,"
		             "cust_flat,cust_dict,cust_generic,order_flat,order_dict,order_generic,line_flat,line_dict,line_generic\n";

		int rows = 0;
		double total_reference_median_us = 0.0;
		double total_accelerator_median_us = 0.0;
		PhaseProfile last_profile;

		for (const auto &entry : accelerator_codegen::test_support::kQ3Manifest) {
			if (!MatchesFilter(entry.label, filters)) {
				continue;
			}

			const auto reference_sql = ReadFile(entry.sql_path);
			const auto accelerator_sql = AcceleratorQ3Sql(entry);
			const auto reference_stats = BenchmarkQuery(connection, reference_sql, warmup_iterations, measured_iterations);
			const auto accelerator_stats =
			    BenchmarkQuery(connection, accelerator_sql, warmup_iterations, measured_iterations);
			accelerator_codegen::Q3Parameters parameters;
			parameters.market_segment = entry.market_segment;
			parameters.cutoff_date = duckdb::Date::FromString(entry.cutoff_date);
			const auto profile = ProfileQ3Execution(connection, parameters);
			last_profile = profile;
			const auto speedup_mean = reference_stats.mean_us / accelerator_stats.mean_us;
			const auto speedup_median = reference_stats.median_us / accelerator_stats.median_us;

			rows += 1;
			total_reference_median_us += reference_stats.median_us;
			total_accelerator_median_us += accelerator_stats.median_us;

			const auto &c = profile.counters;
			std::cout << entry.label << ',' << std::fixed << std::setprecision(2) << reference_stats.mean_us << ','
			          << reference_stats.median_us << ',' << accelerator_stats.mean_us << ','
			          << accelerator_stats.median_us << ',' << speedup_mean << ',' << speedup_median << ','
			          << profile.customer_scan_us << ',' << profile.orders_scan_us << ',' << profile.lineitem_scan_us
			          << ',' << profile.finalize_us << ',' << c.customer_state_bytes << ',' << c.orders_state_bytes
			          << ',' << c.qualifying_customers << ',' << c.qualifying_orders << ',' << c.qualifying_lineitems
			          << ',' << c.touched_orders << ',' << c.aggregated_orders << ','
			          << c.customer.flat_fast_path_chunks << ',' << c.customer.dict_fast_path_chunks << ','
			          << c.customer.generic_fallback_chunks << ',' << c.orders.flat_fast_path_chunks << ','
			          << c.orders.dict_fast_path_chunks << ',' << c.orders.generic_fallback_chunks << ','
			          << c.lineitem.flat_fast_path_chunks << ',' << c.lineitem.dict_fast_path_chunks << ','
			          << c.lineitem.generic_fallback_chunks << "\n";
		}

		Require(rows > 0, "benchmark filter selected no Q3 instances");
		const auto average_reference_median_us = total_reference_median_us / static_cast<double>(rows);
		const auto average_accelerator_median_us = total_accelerator_median_us / static_cast<double>(rows);
		const auto average_speedup_median = average_reference_median_us / average_accelerator_median_us;
		std::cout << "summary_avg_median," << std::fixed << std::setprecision(2) << average_reference_median_us << ","
		          << average_reference_median_us << "," << average_accelerator_median_us << ","
		          << average_accelerator_median_us << "," << average_speedup_median << "," << average_speedup_median
		          << "\n";

		if (verbose) {
			const auto &c = last_profile.counters;
			std::cerr << "\n=== Q3 profile (last seed) ===\n";
			std::cerr << "toggles: dense_cust=" << c.used_dense_customers << " dense_orders=" << c.used_dense_orders
			          << " dict_fast=" << c.used_dict_fast << " topk_heap=" << c.used_topk_heap
			          << " trust_native=" << c.used_trust_native << " shared_sel=" << c.used_shared_sel
			          << " run_cache=" << c.used_run_cache << " slot_map=" << c.used_slot_map
			          << " hot_cold=" << c.used_hot_cold << " base_residual=" << c.used_base_residual
			          << " cust_partial=" << c.used_cust_partial << " slot_absent0=" << c.used_slot_absent0
			          << " slot_reserve=" << c.used_slot_reserve << "\n";
			std::cerr << "run_cache: probes=" << c.order_probes << " avoided=" << c.order_probes_avoided
			          << " runs=" << c.order_runs << " run_rows=" << c.order_run_rows
			          << " agg_writes=" << c.aggregate_writes << " avg_run="
			          << (c.order_runs ? (double)c.order_run_rows / (double)c.order_runs : 0.0) << "\n";
			std::cerr << "residual: checks=" << c.residual_date_checks << " skips=" << c.residual_date_skips
			          << " slots=" << c.slot_count << "\n";
			std::cerr << "slot_map: allocs=" << c.slot_allocs << " pages_est=" << c.slot_map_pages_est
			          << " init_ns=" << c.slot_map_init_ns << " upsert_ns=" << c.orders_upsert_ns << "\n";
			std::cerr << "phases_us: customer=" << last_profile.customer_scan_us
			          << " orders=" << last_profile.orders_scan_us << " orders_next=" << last_profile.orders_next_us
			          << " orders_consume=" << last_profile.orders_consume_us
			          << " lineitem=" << last_profile.lineitem_scan_us
			          << " lineitem_next=" << last_profile.lineitem_next_us
			          << " lineitem_consume=" << last_profile.lineitem_consume_us
			          << " finalize=" << last_profile.finalize_us << " total=" << last_profile.total_us << "\n";
			std::cerr << "kernel_ns: customer=" << c.customer_consume_ns << " orders=" << c.orders_consume_ns
			          << " lineitem=" << c.lineitem_consume_ns << " finalize=" << c.finalize_ns << "\n";
			std::cerr << "memory: cust_bytes=" << c.customer_state_bytes << " orders_bytes=" << c.orders_state_bytes
			          << " cust_overflow=" << c.customer_overflow << " orders_overflow=" << c.orders_overflow << "\n";
			PrintLayout("c_mktsegment", c.layout_c_mktsegment);
			PrintLayout("c_custkey", c.layout_c_custkey);
			PrintLayout("o_orderdate", c.layout_o_orderdate);
			PrintLayout("o_orderkey", c.layout_o_orderkey);
			PrintLayout("o_custkey", c.layout_o_custkey);
			PrintLayout("o_shippriority", c.layout_o_shippriority);
			PrintLayout("l_shipdate", c.layout_l_shipdate);
			PrintLayout("l_orderkey", c.layout_l_orderkey);
			PrintLayout("l_extendedprice", c.layout_l_extendedprice);
			PrintLayout("l_discount", c.layout_l_discount);
		}
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_q3 failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
