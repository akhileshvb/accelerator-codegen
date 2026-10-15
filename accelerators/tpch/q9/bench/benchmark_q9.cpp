#include "eval/tests/test_support/db_paths.hpp"
#include "backends/duckdb/engine/duckdb_scan_adapter.hpp"
#include "backends/duckdb/api/register.hpp"
#include "tpch/q9/duckdb/base/query.hpp"
#include "tpch/q9/tests/q9_manifest.hpp"

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

std::string EscapeSqlLiteral(const std::string &value) {
	std::string escaped;
	escaped.reserve(value.size());
	for (char ch : value) {
		if (ch == '\'') {
			escaped += "''";
		} else {
			escaped.push_back(ch);
		}
	}
	return escaped;
}

std::string ReferenceQ9Sql(const accelerator_codegen::test_support::Q9ManifestEntry &entry) {
	return ReadFile(entry.sql_path);
}

std::string AcceleratorQ9Sql(const accelerator_codegen::test_support::Q9ManifestEntry &entry) {
	return std::string("SELECT * FROM accel_tpch_q9('") + EscapeSqlLiteral(entry.color) + "')";
}

std::vector<std::string> MaterializeOrderedRows(duckdb::MaterializedQueryResult &result) {
	std::vector<std::string> rows;
	rows.reserve(result.RowCount());
	for (duckdb::idx_t row = 0; row < result.RowCount(); row++) {
		std::string encoded;
		for (duckdb::idx_t col = 0; col < result.ColumnCount(); col++) {
			if (col != 0) {
				encoded += " | ";
			}
			encoded += result.GetValue(col, row).ToString();
		}
		rows.push_back(std::move(encoded));
	}
	return rows;
}

void RequireEqualResults(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
                         const std::string &accelerator_sql) {
	auto expected = connection.Query(reference_sql);
	auto actual = connection.Query(accelerator_sql);
	Require(expected != nullptr && !expected->HasError(), label + ": reference failed: " +
	                                                          (expected ? expected->GetError() : "null"));
	Require(actual != nullptr && !actual->HasError(),
	        label + ": accelerator failed: " + (actual ? actual->GetError() : "null"));
	auto &expected_mat = expected->Cast<duckdb::MaterializedQueryResult>();
	auto &actual_mat = actual->Cast<duckdb::MaterializedQueryResult>();
	Require(expected->names == actual->names, label + ": names differ");
	Require(expected->types == actual->types, label + ": types differ");
	Require(MaterializeOrderedRows(expected_mat) == MaterializeOrderedRows(actual_mat), label + ": rows differ");
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
	accelerator_codegen::Q9ProfileCounters counters;
	double part_scan_us = 0.0;
	double supplier_scan_us = 0.0;
	double nation_scan_us = 0.0;
	double orders_scan_us = 0.0;
	double partsupp_scan_us = 0.0;
	double lineitem_scan_us = 0.0;
	double lineitem_next_us = 0.0;
	double lineitem_consume_us = 0.0;
	double finalize_us = 0.0;
	double total_us = 0.0;
};

PhaseProfile ProfileQ9Execution(duckdb::Connection &connection, const accelerator_codegen::Q9Parameters &parameters) {
	PhaseProfile profile;
	WithTransaction(connection, [&](duckdb::ClientContext &context) {
		const auto total_start = Clock::now();
		auto part_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ9PartScanSpec(parameters));
		auto supplier_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ9SupplierScanSpec(parameters));
		auto nation_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ9NationScanSpec(parameters));
		auto orders_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ9OrdersScanSpec(parameters));
		auto partsupp_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ9PartsuppScanSpec(parameters));
		auto lineitem_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ9LineitemScanSpec(parameters));

		auto kernel = accelerator_codegen::CreateQ9ProfilingKernel(parameters, profile.counters);

		auto timed_consume = [&](accelerator_codegen::Q9InputId input_id, duckdb::unique_ptr<duckdb::FunctionData> &scan,
		                         double &scan_us) {
			const auto start = Clock::now();
			auto source = accelerator_codegen::CreateNativeChunkSource(context, *scan);
			while (source->Next()) {
				kernel->ConsumeInput(static_cast<duckdb::idx_t>(input_id), source->Chunk());
			}
			scan_us = static_cast<double>(
			    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
		};

		timed_consume(accelerator_codegen::Q9InputId::PART, part_scan, profile.part_scan_us);
		timed_consume(accelerator_codegen::Q9InputId::SUPPLIER, supplier_scan, profile.supplier_scan_us);
		timed_consume(accelerator_codegen::Q9InputId::NATION, nation_scan, profile.nation_scan_us);
		timed_consume(accelerator_codegen::Q9InputId::ORDERS, orders_scan, profile.orders_scan_us);
		timed_consume(accelerator_codegen::Q9InputId::PARTSUPP, partsupp_scan, profile.partsupp_scan_us);

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
			kernel->ConsumeInput(static_cast<duckdb::idx_t>(accelerator_codegen::Q9InputId::LINEITEM),
			                     lineitem_source->Chunk());
			profile.lineitem_consume_us += static_cast<double>(
			    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - consume_start).count());
		}
		profile.lineitem_scan_us = static_cast<double>(
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - lineitem_start).count());

		auto finalize_start = Clock::now();
		kernel->Finalize();
		duckdb::DataChunk output;
		output.Initialize(context, {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT,
		                            duckdb::LogicalType::DECIMAL(38, 4)});
		while (kernel->ProduceOutput(output)) {
			output.Reset();
		}
		profile.finalize_us = static_cast<double>(
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - finalize_start).count());
		profile.total_us =
		    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - total_start).count());
	});
	return profile;
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

		std::cout << "label,duckdb_mean_us,duckdb_median_us,accelerator_mean_us,accelerator_median_us,speedup_mean,"
		             "speedup_median,part_us,supplier_us,nation_us,orders_us,partsupp_us,lineitem_us,"
		             "lineitem_next_us,lineitem_consume_us,finalize_us,"
		             "part_rows,part_selected,supplier_rows,orders_rows,partsupp_selected,lineitem_rows,"
		             "lineitem_survivors,groups,"
		             "selected_parts_bytes,supplier_map_bytes,nation_map_bytes,orders_map_bytes,"
		             "partsupp_map_bytes,aggregate_bytes\n";

		int rows = 0;
		double total_duckdb_median_us = 0.0;
		double total_accelerator_median_us = 0.0;
		PhaseProfile last_profile;

		for (const auto &entry : accelerator_codegen::test_support::kQ9Manifest) {
			if (!MatchesFilter(entry.label, filters)) {
				continue;
			}

			const auto reference_sql = ReferenceQ9Sql(entry);
			const auto accelerator_sql = AcceleratorQ9Sql(entry);

			RequireEqualResults(connection, std::string(entry.label) + "_correctness_gate", reference_sql,
			                    accelerator_sql);

			TimingStats duckdb_stats;
			TimingStats accelerator_stats;
			if ((rows % 2) == 0) {
				duckdb_stats = BenchmarkQuery(connection, reference_sql, warmup_iterations, measured_iterations);
				accelerator_stats =
				    BenchmarkQuery(connection, accelerator_sql, warmup_iterations, measured_iterations);
			} else {
				accelerator_stats =
				    BenchmarkQuery(connection, accelerator_sql, warmup_iterations, measured_iterations);
				duckdb_stats = BenchmarkQuery(connection, reference_sql, warmup_iterations, measured_iterations);
			}

			accelerator_codegen::Q9Parameters parameters;
			parameters.color = entry.color;
			const auto profile = ProfileQ9Execution(connection, parameters);
			last_profile = profile;

			const auto speedup_mean = duckdb_stats.mean_us / accelerator_stats.mean_us;
			const auto speedup_median = duckdb_stats.median_us / accelerator_stats.median_us;

			rows += 1;
			total_duckdb_median_us += duckdb_stats.median_us;
			total_accelerator_median_us += accelerator_stats.median_us;

			const auto &c = profile.counters;
			std::cout << entry.label << ',' << std::fixed << std::setprecision(2) << duckdb_stats.mean_us << ','
			          << duckdb_stats.median_us << ',' << accelerator_stats.mean_us << ',' << accelerator_stats.median_us
			          << ',' << speedup_mean << ',' << speedup_median << ',' << profile.part_scan_us << ','
			          << profile.supplier_scan_us << ',' << profile.nation_scan_us << ',' << profile.orders_scan_us
			          << ',' << profile.partsupp_scan_us << ',' << profile.lineitem_scan_us << ','
			          << profile.lineitem_next_us << ',' << profile.lineitem_consume_us << ',' << profile.finalize_us
			          << ',' << c.part_rows << ',' << c.part_selected << ',' << c.supplier_rows << ',' << c.orders_rows
			          << ',' << c.partsupp_selected << ',' << c.lineitem_rows << ',' << c.lineitem_survivors << ','
			          << c.group_count << ',' << c.selected_parts_bytes << ',' << c.supplier_map_bytes << ','
			          << c.nation_map_bytes << ',' << c.orders_map_bytes << ',' << c.partsupp_map_bytes << ','
			          << c.aggregate_bytes << "\n";
		}

		Require(rows > 0, "benchmark filter selected no Q9 instances");
		const auto average_duckdb_median_us = total_duckdb_median_us / static_cast<double>(rows);
		const auto average_accelerator_median_us = total_accelerator_median_us / static_cast<double>(rows);
		const auto average_speedup_median = average_duckdb_median_us / average_accelerator_median_us;
		std::cout << "summary_avg_median," << std::fixed << std::setprecision(2) << average_duckdb_median_us << ","
		          << average_duckdb_median_us << "," << average_accelerator_median_us << ","
		          << average_accelerator_median_us << "," << average_speedup_median << "," << average_speedup_median
		          << "\n";

		if (verbose) {
			const auto &c = last_profile.counters;
			std::cerr << "\n=== Q9 profile (last seed) ===\n";
			std::cerr << "toggles: dense_orders=" << c.used_dense_orders << " dense_supplier=" << c.used_dense_supplier
			          << " part_bitmap=" << c.used_part_bitmap << " packed_partsupp=" << c.used_packed_partsupp
			          << " flat_lineitem=" << c.used_flat_lineitem << " dense_agg=" << c.used_dense_agg << "\n";
			std::cerr << "phases_us: part=" << last_profile.part_scan_us << " supplier=" << last_profile.supplier_scan_us
			          << " nation=" << last_profile.nation_scan_us << " orders=" << last_profile.orders_scan_us
			          << " partsupp=" << last_profile.partsupp_scan_us << " lineitem=" << last_profile.lineitem_scan_us
			          << " lineitem_next=" << last_profile.lineitem_next_us
			          << " lineitem_consume=" << last_profile.lineitem_consume_us
			          << " finalize=" << last_profile.finalize_us << " total=" << last_profile.total_us << "\n";
			std::cerr << "orders_sub_ns: init=" << c.orders_init_ns << " format=" << c.orders_format_ns
			          << " insert=" << c.orders_insert_ns << " overflow=" << c.orders_overflow
			          << " domain=[" << c.orderkey_min << "," << c.orderkey_max << "]\n";
			std::cerr << "partsupp_sub_ns: format=" << c.partsupp_format_ns << " insert=" << c.partsupp_insert_ns
			          << " part_hit=" << c.partsupp_part_hit << " part_miss=" << c.partsupp_part_miss << "\n";
			std::cerr << "lineitem_sub_ns: format=" << c.lineitem_format_ns << " probe=" << c.lineitem_probe_ns
			          << " flat_chunks=" << c.lineitem_flat_chunks << " generic_chunks=" << c.lineitem_generic_chunks
			          << " shared_sel=" << c.shared_sel_chunks << "\n";
			std::cerr << "line_hits: part=" << c.line_part_hit << "/" << (c.line_part_hit + c.line_part_miss)
			          << " supplier=" << c.line_supplier_hit << "/" << (c.line_supplier_hit + c.line_supplier_miss)
			          << " order=" << c.line_order_hit << "/" << (c.line_order_hit + c.line_order_miss)
			          << " partsupp=" << c.line_partsupp_hit << "/" << (c.line_partsupp_hit + c.line_partsupp_miss)
			          << "\n";
			std::cerr << "kernel_ns: part=" << c.part_consume_ns << " supplier=" << c.supplier_consume_ns
			          << " nation=" << c.nation_consume_ns << " orders=" << c.orders_consume_ns
			          << " partsupp=" << c.partsupp_consume_ns << " lineitem=" << c.lineitem_consume_ns
			          << " finalize=" << c.finalize_ns << "\n";
			std::cerr << "rows: part=" << c.part_rows << " selected_parts=" << c.part_selected
			          << " supplier=" << c.supplier_rows << " nation=" << c.nation_rows << " orders=" << c.orders_rows
			          << " partsupp_in=" << c.partsupp_rows << " partsupp_sel=" << c.partsupp_selected
			          << " lineitem=" << c.lineitem_rows << " survivors=" << c.lineitem_survivors
			          << " groups=" << c.group_count << "\n";
			std::cerr << "memory_est: parts=" << c.selected_parts_bytes << " supplier=" << c.supplier_map_bytes
			          << " nation=" << c.nation_map_bytes << " orders=" << c.orders_map_bytes
			          << " partsupp=" << c.partsupp_map_bytes << " agg=" << c.aggregate_bytes << "\n";
			auto print_layout = [](const char *name, const accelerator_codegen::Q9LayoutHistogram &h) {
				std::cerr << "  " << name << ": flat=" << h.flat_all_valid << " flat_null=" << h.flat_nullable
				          << " dict=" << h.dict_flat_all_valid << " dict_null=" << h.dict_flat_nullable
				          << " const=" << h.constant_valid << " other=" << h.other << "\n";
			};
			std::cerr << "layouts:\n";
			print_layout("o_orderkey", c.layout_o_orderkey);
			print_layout("o_orderdate", c.layout_o_orderdate);
			print_layout("l_partkey", c.layout_l_partkey);
			print_layout("l_suppkey", c.layout_l_suppkey);
			print_layout("l_orderkey", c.layout_l_orderkey);
			print_layout("l_quantity", c.layout_l_quantity);
			print_layout("l_extendedprice", c.layout_l_extendedprice);
			print_layout("l_discount", c.layout_l_discount);
			print_layout("ps_partkey", c.layout_ps_partkey);
			print_layout("p_name", c.layout_p_name);
		}
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_q9 failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
