#include "backends/duckdb/engine/duckdb_scan_adapter.hpp"
#include "backends/duckdb/api/register.hpp"
#include "tpch/q3/duckdb/base/query.hpp"
#include "tpch/q3/tests/q3_manifest.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
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
	return env_path && *env_path ? env_path : "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
}

int GetEnvInt(const char *name, int fallback) {
	const char *value = std::getenv(name);
	return value && *value ? std::stoi(value) : fallback;
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

std::vector<std::string> SplitCommaSeparated(const char *input) {
	std::vector<std::string> result;
	std::stringstream stream(input ? input : "");
	std::string item;
	while (std::getline(stream, item, ',')) {
		if (!item.empty()) {
			result.push_back(item);
		}
	}
	return result;
}

bool MatchesFilter(const std::string &label, const std::vector<std::string> &filters) {
	return filters.empty() || std::find(filters.begin(), filters.end(), label) != filters.end();
}

std::string EscapeSqlLiteral(const std::string &value) {
	std::string escaped;
	for (const char character : value) {
		escaped += character == '\'' ? "''" : std::string(1, character);
	}
	return escaped;
}

std::string AcceleratorSql(const accelerator_codegen::test_support::Q3ManifestEntry &entry) {
	return "SELECT * FROM accel_tpch_q3('" + EscapeSqlLiteral(entry.market_segment) + "', DATE '" +
	       entry.cutoff_date + "')";
}

std::vector<std::string> MaterializeOrderedRows(duckdb::MaterializedQueryResult &result) {
	std::vector<std::string> rows;
	rows.reserve(result.RowCount());
	for (duckdb::idx_t row = 0; row < result.RowCount(); row++) {
		std::string encoded;
		for (duckdb::idx_t column = 0; column < result.ColumnCount(); column++) {
			if (column != 0) {
				encoded += " | ";
			}
			encoded += result.GetValue(column, row).ToString();
		}
		rows.push_back(std::move(encoded));
	}
	return rows;
}

void RequireEqualResults(duckdb::Connection &connection, const std::string &label,
	                     const std::string &reference_sql, const std::string &accelerator_sql) {
	auto expected = connection.Query(reference_sql);
	auto actual = connection.Query(accelerator_sql);
	Require(expected != nullptr && !expected->HasError(), label + " reference query failed");
	Require(actual != nullptr && !actual->HasError(), label + " accelerator query failed");
	auto &expected_result = expected->Cast<duckdb::MaterializedQueryResult>();
	auto &actual_result = actual->Cast<duckdb::MaterializedQueryResult>();
	Require(expected_result.ColumnCount() == actual_result.ColumnCount(), label + ": column count mismatch");
	Require(expected_result.RowCount() == actual_result.RowCount(), label + ": row count mismatch");
	Require(MaterializeOrderedRows(expected_result) == MaterializeOrderedRows(actual_result),
	        label + ": ordered rows mismatch");
}

void ExecuteAndConsume(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Require(result != nullptr && !result->HasError(), "benchmark query failed");
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	std::size_t bytes_seen = 0;
	for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
		for (duckdb::idx_t column = 0; column < materialized.ColumnCount(); column++) {
			bytes_seen += materialized.GetValue(column, row).ToString().size();
		}
	}
	Require(bytes_seen != static_cast<std::size_t>(-1), "unreachable consumption guard");
}

struct TimingStats {
	double mean_us;
	double median_us;
	double mad_us;
};

double Median(std::vector<double> values) {
	std::sort(values.begin(), values.end());
	const auto middle = values.size() / 2;
	return values.size() % 2 == 0 ? (values[middle - 1] + values[middle]) / 2.0 : values[middle];
}

TimingStats Summarize(const std::vector<double> &samples) {
	Require(!samples.empty(), "no timing samples");
	double sum = 0.0;
	for (const double sample : samples) {
		sum += sample;
	}
	const double median = Median(samples);
	std::vector<double> deviations;
	deviations.reserve(samples.size());
	for (const double sample : samples) {
		deviations.push_back(std::abs(sample - median));
	}
	return {sum / static_cast<double>(samples.size()), median, Median(std::move(deviations))};
}

struct DualTiming {
	TimingStats reference;
	TimingStats accelerator;
};

DualTiming BenchmarkPair(duckdb::Connection &connection, const std::string &reference_sql,
	                     const std::string &accelerator_sql, int warmups, int iterations) {
	Require(warmups >= 0 && iterations > 0, "benchmark iteration counts must be valid");
	for (int iteration = 0; iteration < warmups; iteration++) {
		ExecuteAndConsume(connection, iteration % 2 == 0 ? reference_sql : accelerator_sql);
		ExecuteAndConsume(connection, iteration % 2 == 0 ? accelerator_sql : reference_sql);
	}
	std::vector<double> reference_samples;
	std::vector<double> accelerator_samples;
	auto measure = [&](const std::string &sql, std::vector<double> &samples) {
		const auto start = Clock::now();
		ExecuteAndConsume(connection, sql);
		const auto end = Clock::now();
		samples.push_back(std::chrono::duration<double, std::micro>(end - start).count());
	};
	for (int iteration = 0; iteration < iterations; iteration++) {
		if (iteration % 2 == 0) {
			measure(reference_sql, reference_samples);
			measure(accelerator_sql, accelerator_samples);
		} else {
			measure(accelerator_sql, accelerator_samples);
			measure(reference_sql, reference_samples);
		}
	}
	return {Summarize(reference_samples), Summarize(accelerator_samples)};
}

template <class Callback>
void WithTransaction(duckdb::Connection &connection, Callback &&callback) {
	const bool started = !connection.HasActiveTransaction();
	if (started) {
		connection.BeginTransaction();
	}
	try {
		callback(*connection.context);
		if (started) {
			connection.Commit();
		}
	} catch (...) {
		if (started && connection.HasActiveTransaction()) {
			connection.Rollback();
		}
		throw;
	}
}

struct Q3BenchProfile {
	accelerator_codegen::Q3ProfileCounters kernel;
	int64_t customer_scan_us = 0;
	int64_t customer_consume_us = 0;
	int64_t orders_scan_us = 0;
	int64_t orders_consume_us = 0;
	int64_t lineitem_scan_us = 0;
};

Q3BenchProfile ProfileExecution(duckdb::Connection &connection,
                                const accelerator_codegen::test_support::Q3ManifestEntry &entry) {
	Q3BenchProfile profile;
	WithTransaction(connection, [&](duckdb::ClientContext &context) {
		accelerator_codegen::Q3Parameters parameters;
		parameters.market_segment = entry.market_segment;
		parameters.cutoff_date = duckdb::Date::FromString(entry.cutoff_date);

		auto customer_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ3CustomerScanSpec(parameters));
		auto orders_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ3OrdersScanSpec(parameters));
		auto lineitem_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ3LineitemScanSpec(parameters));
		auto kernel = accelerator_codegen::CreateQ3ProfilingKernel(parameters, profile.kernel);

		auto drain = [&](accelerator_codegen::Q3InputId input_id, const duckdb::FunctionData &scan,
		                 int64_t &scan_us, int64_t *consume_us) {
			auto source = accelerator_codegen::CreateNativeChunkSource(context, scan);
			while (true) {
				const auto scan_start = Clock::now();
				const bool has_chunk = source->Next();
				scan_us += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - scan_start).count();
				if (!has_chunk) {
					break;
				}
				const auto consume_start = Clock::now();
				kernel->ConsumeInput(static_cast<duckdb::idx_t>(input_id), source->Chunk());
				if (consume_us) {
					*consume_us +=
					    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - consume_start).count();
				}
			}
		};
		drain(accelerator_codegen::Q3InputId::CUSTOMER, *customer_scan, profile.customer_scan_us,
		      &profile.customer_consume_us);
		drain(accelerator_codegen::Q3InputId::ORDERS, *orders_scan, profile.orders_scan_us,
		      &profile.orders_consume_us);
		drain(accelerator_codegen::Q3InputId::LINEITEM, *lineitem_scan, profile.lineitem_scan_us, nullptr);
		kernel->Finalize();
		duckdb::DataChunk output;
		output.Initialize(context, {duckdb::LogicalType::BIGINT, duckdb::LogicalType::DECIMAL(38, 4),
		                            duckdb::LogicalType::DATE, duckdb::LogicalType::INTEGER});
		while (kernel->ProduceOutput(output)) {
			output.Reset();
		}
	});
	return profile;
}

void PrintCounters(const Q3BenchProfile &profile) {
	const auto &k = profile.kernel;
	std::cout << ',' << k.customer.total_rows << ',' << k.qualifying_customers << ','
	          << k.customer.generic_fallback_rows << ',' << profile.customer_scan_us << ','
	          << profile.customer_consume_us << ',' << k.orders.total_rows << ',' << k.qualifying_orders << ','
	          << k.orders.generic_fallback_rows << ',' << profile.orders_scan_us << ','
	          << profile.orders_consume_us << ',' << k.lineitem.total_rows << ',' << k.qualifying_lineitems << ','
	          << k.lineitem.generic_fallback_rows << ',' << profile.lineitem_scan_us << ','
	          << k.lineitem_consume_ns / 1000 << ',' << k.aggregated_orders << ',' << k.finalize_ns / 1000;
}

} // namespace

int main() {
	try {
		const int warmups = GetEnvInt("ACCEL_BENCH_WARMUP", 3);
		const int iterations = GetEnvInt("ACCEL_BENCH_ITERS", 10);
		const auto filters = SplitCommaSeparated(std::getenv("ACCEL_BENCH_FILTER"));
		duckdb::DuckDB database(GetTpchDbPath().c_str());
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		auto pragma = connection.Query("PRAGMA threads=1");
		Require(pragma != nullptr && !pragma->HasError(), "failed to set PRAGMA threads=1");

		std::cout << "label,reference_mean_us,reference_median_us,reference_mad_us,"
		             "accelerator_mean_us,accelerator_median_us,accelerator_mad_us,speedup_median,"
		             "customer_rows,qualifying_customers,customer_generic_fallback_rows,customer_scan_us,"
		             "customer_consume_us,orders_rows,qualifying_orders,orders_generic_fallback_rows,orders_scan_us,"
		             "orders_consume_us,lineitem_rows,qualifying_lineitems,lineitem_generic_fallback_rows,"
		             "lineitem_scan_us,lineitem_consume_us,aggregated_orders,finalize_us\n";

		int row_count = 0;
		double reference_median_sum = 0.0;
		double accelerator_median_sum = 0.0;
		for (const auto &entry : accelerator_codegen::test_support::kQ3Manifest) {
			if (!MatchesFilter(entry.label, filters)) {
				continue;
			}
			const auto reference_sql = ReadFile(entry.sql_path);
			const auto accelerator_sql = AcceleratorSql(entry);
			RequireEqualResults(connection, entry.label, reference_sql, accelerator_sql);
			const auto timing = BenchmarkPair(connection, reference_sql, accelerator_sql, warmups, iterations);
			const auto counters = ProfileExecution(connection, entry);
			const double speedup = timing.reference.median_us / timing.accelerator.median_us;
			std::cout << entry.label << ',' << std::fixed << std::setprecision(2) << timing.reference.mean_us << ','
			          << timing.reference.median_us << ',' << timing.reference.mad_us << ','
			          << timing.accelerator.mean_us << ',' << timing.accelerator.median_us << ','
			          << timing.accelerator.mad_us << ',' << speedup;
			PrintCounters(counters);
			std::cout << '\n';
			row_count++;
			reference_median_sum += timing.reference.median_us;
			accelerator_median_sum += timing.accelerator.median_us;
		}
		Require(row_count > 0, "benchmark filter selected no Q3 instances");
		const double reference_average = reference_median_sum / row_count;
		const double accelerator_average = accelerator_median_sum / row_count;
		std::cout << "summary_avg_median," << std::fixed << std::setprecision(2) << reference_average << ",,,"
		          << accelerator_average << ",,," << reference_average / accelerator_average << '\n';
		return EXIT_SUCCESS;
	} catch (const std::exception &exception) {
		std::cerr << "benchmark_q3 failed: " << exception.what() << std::endl;
		return EXIT_FAILURE;
	}
}
