//! Fair SQL-vs-accelerator validator for all implemented TPC-H accelerators:
//! Q1, Q3, Q4, Q6, Q9, Q12, Q14.
//! Headline timing: full connection.Query() + typed result consumption on both paths.
//! Correctness is checked before timing; mismatched results abort the run.
//! Production paths are hardcoded in kernels (no ambient ACCEL_* dependence).

#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "tpch/q1/base/query.hpp"
#include "tpch/q1/base/fused.hpp"
#include "tpch/q3/base/query.hpp"
#include "tpch/q4/base/query.hpp"
#include "tpch/q9/base/query.hpp"
#include "tpch/q12/base/query.hpp"
#include "tpch/q14/base/query.hpp"
#include "test_support/q1_manifest.hpp"
#include "test_support/q3_manifest.hpp"
#include "test_support/q4_manifest.hpp"
#include "test_support/q6_manifest.hpp"
#include "test_support/q9_manifest.hpp"
#include "test_support/q12_manifest.hpp"
#include "test_support/q14_manifest.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#if defined(__linux__)
#include <unistd.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;

std::string GetTpchDbPath() {
	const char *env_path = std::getenv("ACCEL_TPCH_DB");
	if (env_path && *env_path) {
		return env_path;
	}
	return "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
}

int InferScaleFactor(const std::string &db_path, int64_t lineitem_count) {
	const char *env_sf = std::getenv("ACCEL_TPCH_SCALE");
	if (env_sf && *env_sf) {
		return std::stoi(env_sf);
	}
	if (db_path.find("sf10") != std::string::npos || db_path.find("SF10") != std::string::npos) {
		return 10;
	}
	if (db_path.find("sf100") != std::string::npos || db_path.find("SF100") != std::string::npos) {
		return 100;
	}
	if (lineitem_count == 59986052) {
		return 10;
	}
	if (lineitem_count == 6001215) {
		return 1;
	}
	return 0;
}

int64_t ExpectedLineitemCount(int scale_factor) {
	switch (scale_factor) {
	case 1:
		return 6001215;
	case 10:
		return 59986052;
	default:
		return -1;
	}
}

std::vector<std::string> SplitCommaSeparated(const std::string &value) {
	std::vector<std::string> parts;
	std::stringstream stream(value);
	std::string item;
	while (std::getline(stream, item, ',')) {
		if (!item.empty()) {
			parts.push_back(item);
		}
	}
	return parts;
}

bool MatchesFilter(const std::string &label, const std::vector<std::string> &filters) {
	if (filters.empty()) {
		return true;
	}
	return std::find(filters.begin(), filters.end(), label) != filters.end();
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

enum class AcceleratorMode { Single, Parallel };

AcceleratorMode ParseAcceleratorMode(const std::string &value) {
	if (value.empty() || value == "single") {
		return AcceleratorMode::Single;
	}
	if (value == "parallel") {
		return AcceleratorMode::Parallel;
	}
	throw std::runtime_error("invalid accelerator mode '" + value + "' (expected single|parallel)");
}

std::string AccelFunctionName(const char *base, AcceleratorMode mode) {
	if (mode == AcceleratorMode::Parallel) {
		return std::string(base) + "_parallel";
	}
	return base;
}

//! Parse optional CLI: --accelerator-mode {single|parallel} --threads N
void ParseValidatedCli(int argc, char **argv, AcceleratorMode &mode, int &threads) {
	mode = ParseAcceleratorMode(GetEnvString("ACCEL_ACCELERATOR_MODE"));
	threads = GetEnvInt("ACCEL_BENCH_THREADS", 1);
	for (int i = 1; i < argc; i++) {
		const std::string arg = argv[i];
		if (arg == "--accelerator-mode") {
			if (i + 1 >= argc) {
				throw std::runtime_error("--accelerator-mode requires a value");
			}
			mode = ParseAcceleratorMode(argv[++i]);
		} else if (arg == "--threads") {
			if (i + 1 >= argc) {
				throw std::runtime_error("--threads requires a value");
			}
			threads = std::stoi(argv[++i]);
			if (threads < 1 || threads > 256) {
				throw std::runtime_error("--threads out of range");
			}
		} else if (arg == "--help" || arg == "-h") {
			std::cerr << "Usage: benchmark_tpch_validated [--accelerator-mode single|parallel] [--threads N]\n"
			             "Env: ACCEL_ACCELERATOR_MODE, ACCEL_BENCH_THREADS, ACCEL_TPCH_DB, ACCEL_TPCH_SCALE, "
			             "ACCEL_BENCH_WARMUP, ACCEL_BENCH_ITERS, ACCEL_BENCH_SESSION, ACCEL_BENCH_FILTER\n";
			std::exit(0);
		} else {
			throw std::runtime_error("unknown argument: " + arg);
		}
	}
}

const char *EnvOrUnset(const char *name) {
	const char *value = std::getenv(name);
	return (value && *value) ? value : "<unset>";
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

std::string StripTrailingSemicolon(std::string sql) {
	while (!sql.empty() && (sql.back() == ';' || sql.back() == '\n' || sql.back() == '\r' || sql.back() == ' ')) {
		sql.pop_back();
	}
	return sql;
}

std::unique_ptr<duckdb::QueryResult> RunQuery(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Require(result != nullptr, "query returned null result");
	Require(!result->HasError(), result->GetError());
	return result;
}

void ConsumeResult(duckdb::MaterializedQueryResult &materialized) {
	volatile std::size_t sink = 0;
	for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
		for (duckdb::idx_t col = 0; col < materialized.ColumnCount(); col++) {
			const auto value = materialized.GetValue(col, row);
			sink += value.IsNull() ? 0 : 1;
			sink += static_cast<std::size_t>(value.type().id());
		}
	}
	Require(sink != static_cast<std::size_t>(-1), "unreachable consume guard");
}

void ExecuteAndConsume(duckdb::Connection &connection, const std::string &sql) {
	auto result = RunQuery(connection, sql);
	ConsumeResult(result->Cast<duckdb::MaterializedQueryResult>());
}

std::string CanonicalValue(const duckdb::Value &value) {
	if (value.IsNull()) {
		return "NULL";
	}
	switch (value.type().id()) {
	case duckdb::LogicalTypeId::DECIMAL:
		return value.GetValueUnsafe<duckdb::hugeint_t>().ToString() + "@" + value.type().ToString();
	case duckdb::LogicalTypeId::DOUBLE: {
		// Doubles are compared with relative tolerance in CompareRows, not via this string.
		std::ostringstream out;
		out << std::setprecision(17) << value.GetValue<double>();
		return out.str();
	}
	default:
		return value.ToString() + "@" + value.type().ToString();
	}
}

bool DoublesClose(double expected, double actual) {
	const double diff = std::fabs(expected - actual);
	const double scale = std::max(1.0, std::fabs(expected));
	return diff <= 1e-12 * scale;
}

bool ValuesEqual(const duckdb::Value &expected, const duckdb::Value &actual) {
	if (expected.type() != actual.type()) {
		return false;
	}
	if (expected.IsNull() != actual.IsNull()) {
		return false;
	}
	if (expected.IsNull()) {
		return true;
	}
	switch (expected.type().id()) {
	case duckdb::LogicalTypeId::VARCHAR:
		return expected.GetValue<std::string>() == actual.GetValue<std::string>();
	case duckdb::LogicalTypeId::DECIMAL:
		return expected.GetValueUnsafe<duckdb::hugeint_t>() == actual.GetValueUnsafe<duckdb::hugeint_t>();
	case duckdb::LogicalTypeId::DOUBLE:
		return DoublesClose(expected.GetValue<double>(), actual.GetValue<double>());
	case duckdb::LogicalTypeId::BIGINT:
		return expected.GetValue<int64_t>() == actual.GetValue<int64_t>();
	case duckdb::LogicalTypeId::INTEGER:
		return expected.GetValue<int32_t>() == actual.GetValue<int32_t>();
	case duckdb::LogicalTypeId::DATE:
		return expected.GetValue<duckdb::date_t>() == actual.GetValue<duckdb::date_t>();
	default:
		return expected.ToString() == actual.ToString();
	}
}

void RequireEqualResults(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
                         const std::string &accelerator_sql, bool canonicalize_q3_ties) {
	std::string expected_sql = reference_sql;
	std::string actual_sql = accelerator_sql;
	if (canonicalize_q3_ties) {
		expected_sql = "SELECT l_orderkey, revenue, o_orderdate, o_shippriority FROM (" +
		               StripTrailingSemicolon(reference_sql) +
		               ") ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority";
		actual_sql = "SELECT l_orderkey, revenue, o_orderdate, o_shippriority FROM (" +
		             StripTrailingSemicolon(accelerator_sql) +
		             ") ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority";
	}

	auto expected = RunQuery(connection, expected_sql);
	auto actual = RunQuery(connection, actual_sql);
	auto &expected_mat = expected->Cast<duckdb::MaterializedQueryResult>();
	auto &actual_mat = actual->Cast<duckdb::MaterializedQueryResult>();

	Require(expected->types == actual->types, label + ": column types differ");
	Require(expected_mat.RowCount() == actual_mat.RowCount(), label + ": row counts differ");
	Require(expected_mat.ColumnCount() == actual_mat.ColumnCount(), label + ": column counts differ");

	for (duckdb::idx_t row = 0; row < expected_mat.RowCount(); row++) {
		for (duckdb::idx_t col = 0; col < expected_mat.ColumnCount(); col++) {
			const auto expected_value = expected_mat.GetValue(col, row);
			const auto actual_value = actual_mat.GetValue(col, row);
			if (!ValuesEqual(expected_value, actual_value)) {
				throw std::runtime_error(label + ": value differs at row " + std::to_string(row) + ", col " +
				                         std::to_string(col) + " expected=" + CanonicalValue(expected_value) +
				                         " actual=" + CanonicalValue(actual_value));
			}
		}
	}
}

struct SampleStats {
	double median_us = 0.0;
	double mean_us = 0.0;
	double min_us = 0.0;
	double max_us = 0.0;
	double mad_us = 0.0;
	double cv = 0.0;
	std::vector<double> samples_us;
};

double MedianOf(std::vector<double> values) {
	Require(!values.empty(), "median of empty");
	std::sort(values.begin(), values.end());
	const auto middle = values.size() / 2;
	return values.size() % 2 == 0 ? (values[middle - 1] + values[middle]) / 2.0 : values[middle];
}

SampleStats Summarize(std::vector<double> samples_us) {
	Require(!samples_us.empty(), "empty samples");
	SampleStats stats;
	stats.samples_us = samples_us;
	stats.min_us = *std::min_element(samples_us.begin(), samples_us.end());
	stats.max_us = *std::max_element(samples_us.begin(), samples_us.end());
	stats.mean_us = std::accumulate(samples_us.begin(), samples_us.end(), 0.0) / samples_us.size();
	stats.median_us = MedianOf(samples_us);
	std::vector<double> abs_dev;
	abs_dev.reserve(samples_us.size());
	for (const auto sample : samples_us) {
		abs_dev.push_back(std::fabs(sample - stats.median_us));
	}
	stats.mad_us = MedianOf(abs_dev);
	stats.cv = stats.mean_us > 0.0 ? ([&]() {
		double var = 0.0;
		for (const auto sample : samples_us) {
			const double d = sample - stats.mean_us;
			var += d * d;
		}
		var /= static_cast<double>(samples_us.size());
		return std::sqrt(var) / stats.mean_us;
	})()
	                                                   : 0.0;
	return stats;
}

int64_t ReadCurrentRssKb() {
#if defined(__linux__)
	std::ifstream status("/proc/self/status");
	std::string line;
	while (std::getline(status, line)) {
		if (line.rfind("VmRSS:", 0) == 0) {
			std::istringstream in(line.substr(6));
			int64_t kb = 0;
			in >> kb;
			return kb;
		}
	}
#endif
	return 0;
}

struct PairedTiming {
	SampleStats duckdb;
	SampleStats accelerator;
	double first_duckdb_us = 0.0;
	double first_accelerator_us = 0.0;
	int64_t peak_rss_kb = 0;
};

PairedTiming BenchmarkPaired(duckdb::Connection &connection, const std::string &reference_sql,
                             const std::string &accelerator_sql, int warmup_iterations, int measured_iterations,
                             bool accel_first_on_even) {
	int64_t peak_rss_kb = ReadCurrentRssKb();
	auto note_rss = [&]() {
		peak_rss_kb = std::max(peak_rss_kb, ReadCurrentRssKb());
	};

	// Warmups alternate with the same order policy as measured iterations.
	for (int i = 0; i < warmup_iterations; i++) {
		const bool accel_first = accel_first_on_even ? (i % 2 == 0) : (i % 2 == 1);
		if (accel_first) {
			ExecuteAndConsume(connection, accelerator_sql);
			note_rss();
			ExecuteAndConsume(connection, reference_sql);
			note_rss();
		} else {
			ExecuteAndConsume(connection, reference_sql);
			note_rss();
			ExecuteAndConsume(connection, accelerator_sql);
			note_rss();
		}
	}

	std::vector<double> duckdb_samples;
	std::vector<double> accel_samples;
	duckdb_samples.reserve(measured_iterations);
	accel_samples.reserve(measured_iterations);

	for (int i = 0; i < measured_iterations; i++) {
		const bool accel_first = accel_first_on_even ? (i % 2 == 0) : (i % 2 == 1);
		auto time_one = [&](const std::string &sql) {
			const auto start = Clock::now();
			ExecuteAndConsume(connection, sql);
			note_rss();
			const auto end = Clock::now();
			return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
		};
		double duckdb_us = 0.0;
		double accel_us = 0.0;
		if (accel_first) {
			accel_us = time_one(accelerator_sql);
			duckdb_us = time_one(reference_sql);
		} else {
			duckdb_us = time_one(reference_sql);
			accel_us = time_one(accelerator_sql);
		}
		duckdb_samples.push_back(duckdb_us);
		accel_samples.push_back(accel_us);
	}

	PairedTiming timing;
	timing.duckdb = Summarize(duckdb_samples);
	timing.accelerator = Summarize(accel_samples);
	timing.first_duckdb_us = duckdb_samples.front();
	timing.first_accelerator_us = accel_samples.front();
	timing.peak_rss_kb = peak_rss_kb;
	return timing;
}

int64_t VerifyThreads(duckdb::Connection &connection) {
	auto result = RunQuery(connection, "SELECT current_setting('threads')");
	auto &mat = result->Cast<duckdb::MaterializedQueryResult>();
	Require(mat.RowCount() == 1, "threads setting missing");
	return mat.GetValue(0, 0).GetValue<int64_t>();
}

void PrintEnvBanner() {
	std::cerr << "=== validated TPC-H benchmark (production paths; no ACCEL_* required) ===\n";
	std::cerr << "ACCEL_TPCH_DB=" << GetTpchDbPath() << "\n";
	std::cerr << "ACCEL_BENCH_WARMUP=" << EnvOrUnset("ACCEL_BENCH_WARMUP") << "\n";
	std::cerr << "ACCEL_BENCH_ITERS=" << EnvOrUnset("ACCEL_BENCH_ITERS") << "\n";
	std::cerr << "ACCEL_BENCH_SESSION=" << EnvOrUnset("ACCEL_BENCH_SESSION") << "\n";
	std::cerr << "DUCKDB_BITPACKING_NATIVE_FILTER=" << EnvOrUnset("DUCKDB_BITPACKING_NATIVE_FILTER") << "\n";
	std::cerr << "production: Q1FusedScan=" << accelerator_codegen::Q1FusedScanEnabled()
	          << " Q1Aligned=" << accelerator_codegen::Q1FusedAlignedPayloadEnabled()
	          << " Q1DirectIndex=" << accelerator_codegen::Q1FusedDirectIndexEnabled()
	          << " Q4LineitemFused=" << accelerator_codegen::Q4LineitemFusedScanEnabled() << "\n";
	std::cerr << "note: kernel ACCEL_* toggles are compile-time gated (-DACCEL_EXPERIMENTAL_Q*_OPTS)\n";
}

struct CsvRow {
	std::string query;
	std::string seed;
	std::string parameters;
	double duckdb_median_ms = 0.0;
	double accelerator_median_ms = 0.0;
	double speedup = 0.0;
	std::string duckdb_dispersion;
	std::string accelerator_dispersion;
	bool correctness_passed = false;
	std::string transient_state;
	double duckdb_cv = 0.0;
	double accelerator_cv = 0.0;
	double first_duckdb_ms = 0.0;
	double first_accelerator_ms = 0.0;
	std::string order_note;
	int threads = 1;
	std::string accelerator_mode = "single";
	int64_t shared_state_bytes = 0;
	int64_t per_worker_state_bytes = 0;
	int64_t query_local_state_bytes = 0;
	int64_t peak_rss_kb = 0;
};

int64_t ParseTaggedInt64(const std::string &text, const std::string &key) {
	const auto needle = key + "=";
	const auto pos = text.find(needle);
	if (pos == std::string::npos) {
		return 0;
	}
	return std::strtoll(text.c_str() + pos + needle.size(), nullptr, 10);
}

//! Annotate ST profile counters with parallel shared + N×worker estimates.
//! Worker estimates are lower bounds on durable per-worker aggregates (not peak build scratch).
std::string AnnotateMemory(const std::string &query, const std::string &st_state, int threads, bool parallel_mode,
                           int64_t &shared_out, int64_t &per_worker_out, int64_t &query_local_out) {
	shared_out = 0;
	per_worker_out = 0;
	if (query == "Q1") {
		shared_out = 0;
		per_worker_out = parallel_mode ? (64 * 8) : 0;
	} else if (query == "Q6") {
		shared_out = 0;
		per_worker_out = parallel_mode ? 64 : 0;
	} else if (query == "Q3") {
		shared_out = ParseTaggedInt64(st_state, "cust_bytes") + ParseTaggedInt64(st_state, "orders_bytes");
		const auto slots = ParseTaggedInt64(st_state, "slots");
		per_worker_out = parallel_mode ? (slots * (16 + 1)) : 0;
		if (!parallel_mode) {
			// ST revenues live in one aggregate structure counted with slots via orders path.
			query_local_out = shared_out + slots * (16 + 1);
			std::ostringstream out;
			out << st_state << ";threads=" << threads << ";shared_bytes=" << shared_out
			    << ";per_worker_bytes=0;query_local_bytes=" << query_local_out;
			return out.str();
		}
	} else if (query == "Q4") {
		shared_out = ParseTaggedInt64(st_state, "bitmap_bytes");
		per_worker_out = parallel_mode ? (shared_out + 64) : 0;
		if (!parallel_mode) {
			query_local_out = shared_out;
			std::ostringstream out;
			out << st_state << ";threads=" << threads << ";shared_bytes=" << shared_out
			    << ";per_worker_bytes=0;query_local_bytes=" << query_local_out;
			return out.str();
		}
	} else if (query == "Q9") {
		shared_out = ParseTaggedInt64(st_state, "parts") + ParseTaggedInt64(st_state, "supplier") +
		             ParseTaggedInt64(st_state, "nation") + ParseTaggedInt64(st_state, "orders") +
		             ParseTaggedInt64(st_state, "partsupp");
		const auto agg = ParseTaggedInt64(st_state, "agg");
		per_worker_out = parallel_mode ? agg : 0;
		if (!parallel_mode) {
			query_local_out = shared_out + agg;
			std::ostringstream out;
			out << st_state << ";threads=" << threads << ";shared_bytes=" << shared_out
			    << ";per_worker_bytes=0;query_local_bytes=" << query_local_out;
			return out.str();
		}
	} else if (query == "Q12") {
		shared_out = ParseTaggedInt64(st_state, "orders_map_bytes");
		per_worker_out = parallel_mode ? 128 : 0;
		if (!parallel_mode) {
			query_local_out = shared_out;
			std::ostringstream out;
			out << st_state << ";threads=" << threads << ";shared_bytes=" << shared_out
			    << ";per_worker_bytes=0;query_local_bytes=" << query_local_out;
			return out.str();
		}
	} else if (query == "Q14") {
		shared_out = ParseTaggedInt64(st_state, "part_map_bytes");
		per_worker_out = parallel_mode ? 128 : 0;
		if (!parallel_mode) {
			query_local_out = shared_out;
			std::ostringstream out;
			out << st_state << ";threads=" << threads << ";shared_bytes=" << shared_out
			    << ";per_worker_bytes=0;query_local_bytes=" << query_local_out;
			return out.str();
		}
	}
	query_local_out = shared_out + static_cast<int64_t>(threads) * per_worker_out;
	std::ostringstream out;
	out << st_state << ";threads=" << threads << ";shared_bytes=" << shared_out
	    << ";per_worker_bytes=" << per_worker_out << ";query_local_bytes=" << query_local_out;
	return out.str();
}

std::string FormatDispersion(const SampleStats &stats) {
	std::ostringstream out;
	out << std::fixed << std::setprecision(3) << "mad_ms=" << (stats.mad_us / 1000.0) << ";min_ms="
	    << (stats.min_us / 1000.0) << ";max_ms=" << (stats.max_us / 1000.0) << ";cv=" << stats.cv;
	return out.str();
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

std::string MeasureQ3State(duckdb::Connection &connection, const accelerator_codegen::test_support::Q3ManifestEntry &entry) {
	accelerator_codegen::Q3Parameters parameters;
	parameters.market_segment = entry.market_segment;
	parameters.cutoff_date = duckdb::Date::FromString(entry.cutoff_date);
	accelerator_codegen::Q3ProfileCounters counters;
	WithTransaction(connection, [&](duckdb::ClientContext &context) {
		auto customer_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ3CustomerScanSpec(parameters));
		auto orders_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ3OrdersScanSpec(parameters));
		auto lineitem_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ3LineitemScanSpec(parameters));
		auto kernel = accelerator_codegen::CreateQ3ProfilingKernel(parameters, counters);
		auto customer_source = accelerator_codegen::CreateNativeChunkSource(context, *customer_scan);
		while (customer_source->Next()) {
			kernel->ConsumeInput(static_cast<duckdb::idx_t>(accelerator_codegen::Q3InputId::CUSTOMER),
			                     customer_source->Chunk());
		}
		auto orders_source = accelerator_codegen::CreateNativeChunkSource(context, *orders_scan);
		while (orders_source->Next()) {
			kernel->ConsumeInput(static_cast<duckdb::idx_t>(accelerator_codegen::Q3InputId::ORDERS),
			                     orders_source->Chunk());
		}
		auto lineitem_source = accelerator_codegen::CreateNativeChunkSource(context, *lineitem_scan);
		while (lineitem_source->Next()) {
			kernel->ConsumeInput(static_cast<duckdb::idx_t>(accelerator_codegen::Q3InputId::LINEITEM),
			                     lineitem_source->Chunk());
		}
		kernel->Finalize();
	});
	std::ostringstream out;
	out << "cust_bytes=" << counters.customer_state_bytes << ";orders_bytes=" << counters.orders_state_bytes
	    << ";slots=" << counters.slot_count;
	return out.str();
}

std::string MeasureQ4State(duckdb::Connection &connection, const accelerator_codegen::test_support::Q4ManifestEntry &entry) {
	const auto parameters = accelerator_codegen::MakeQ4Parameters(duckdb::Date::FromString(entry.start_date));
	accelerator_codegen::Q4ProfileCounters counters;
	WithTransaction(connection, [&](duckdb::ClientContext &context) {
		auto lineitem_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ4LineitemScanSpec(parameters));
		auto orders_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ4OrdersScanSpec(parameters));
		auto kernel = accelerator_codegen::CreateQ4ProfilingKernel(parameters, counters);
		auto lineitem_source = accelerator_codegen::CreateNativeChunkSource(context, *lineitem_scan);
		while (lineitem_source->Next()) {
			kernel->ConsumeInput(static_cast<duckdb::idx_t>(accelerator_codegen::Q4InputId::LINEITEM),
			                     lineitem_source->Chunk());
		}
		auto orders_source = accelerator_codegen::CreateNativeChunkSource(context, *orders_scan);
		while (orders_source->Next()) {
			kernel->ConsumeInput(static_cast<duckdb::idx_t>(accelerator_codegen::Q4InputId::ORDERS),
			                     orders_source->Chunk());
		}
		kernel->Finalize();
	});
	std::ostringstream out;
	out << "bitmap_bytes=" << counters.set_memory_bytes << ";unique_keys=" << counters.unique_qualifying_orderkeys;
	return out.str();
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

std::string MeasureQ9State(duckdb::Connection &connection, const accelerator_codegen::test_support::Q9ManifestEntry &entry) {
	accelerator_codegen::Q9Parameters parameters;
	parameters.color = entry.color;
	accelerator_codegen::Q9ProfileCounters counters;
	WithTransaction(connection, [&](duckdb::ClientContext &context) {
		auto part = accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ9PartScanSpec(parameters));
		auto supplier =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ9SupplierScanSpec(parameters));
		auto nation =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ9NationScanSpec(parameters));
		auto orders =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ9OrdersScanSpec(parameters));
		auto partsupp =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ9PartsuppScanSpec(parameters));
		auto lineitem =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ9LineitemScanSpec(parameters));
		auto kernel = accelerator_codegen::CreateQ9ProfilingKernel(parameters, counters);
		auto drain = [&](accelerator_codegen::Q9InputId id, const duckdb::FunctionData &bound) {
			auto source = accelerator_codegen::CreateNativeChunkSource(context, bound);
			while (source->Next()) {
				kernel->ConsumeInput(static_cast<duckdb::idx_t>(id), source->Chunk());
			}
		};
		drain(accelerator_codegen::Q9InputId::PART, *part);
		drain(accelerator_codegen::Q9InputId::SUPPLIER, *supplier);
		drain(accelerator_codegen::Q9InputId::NATION, *nation);
		drain(accelerator_codegen::Q9InputId::ORDERS, *orders);
		drain(accelerator_codegen::Q9InputId::PARTSUPP, *partsupp);
		drain(accelerator_codegen::Q9InputId::LINEITEM, *lineitem);
		kernel->Finalize();
	});
	std::ostringstream out;
	out << "parts=" << counters.selected_parts_bytes << ";supplier=" << counters.supplier_map_bytes
	    << ";nation=" << counters.nation_map_bytes << ";orders=" << counters.orders_map_bytes
	    << ";partsupp=" << counters.partsupp_map_bytes << ";agg=" << counters.aggregate_bytes;
	return out.str();
}

std::string MeasureQ12State(duckdb::Connection &connection,
                            const accelerator_codegen::test_support::Q12ManifestEntry &entry) {
	const auto parameters = accelerator_codegen::MakeQ12Parameters(entry.shipmode1, entry.shipmode2,
	                                                               duckdb::Date::FromString(entry.start_date));
	accelerator_codegen::Q12ProfileCounters counters;
	WithTransaction(connection, [&](duckdb::ClientContext &context) {
		auto orders_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ12OrdersScanSpec(parameters));
		auto lineitem_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ12LineitemScanSpec(parameters));
		auto kernel = accelerator_codegen::CreateQ12ProfilingKernel(parameters, counters);
		auto orders_source = accelerator_codegen::CreateNativeChunkSource(context, *orders_scan);
		while (orders_source->Next()) {
			kernel->ConsumeInput(static_cast<duckdb::idx_t>(accelerator_codegen::Q12InputId::ORDERS),
			                     orders_source->Chunk());
		}
		auto lineitem_source = accelerator_codegen::CreateNativeChunkSource(context, *lineitem_scan);
		while (lineitem_source->Next()) {
			kernel->ConsumeInput(static_cast<duckdb::idx_t>(accelerator_codegen::Q12InputId::LINEITEM),
			                     lineitem_source->Chunk());
		}
		kernel->Finalize();
	});
	std::ostringstream out;
	out << "orders_map_bytes=" << counters.orders_map_memory_bytes
	    << ";orders_card=" << counters.orders_map_cardinality;
	return out.str();
}

std::string MeasureQ14State(duckdb::Connection &connection,
                            const accelerator_codegen::test_support::Q14ManifestEntry &entry) {
	const auto parameters = accelerator_codegen::MakeQ14Parameters(duckdb::Date::FromString(entry.start_date));
	accelerator_codegen::Q14ProfileCounters counters;
	WithTransaction(connection, [&](duckdb::ClientContext &context) {
		auto part_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ14PartScanSpec(parameters));
		auto lineitem_scan =
		    accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ14LineitemScanSpec(parameters));
		auto kernel = accelerator_codegen::CreateQ14ProfilingKernel(parameters, counters);
		auto part_source = accelerator_codegen::CreateNativeChunkSource(context, *part_scan);
		while (part_source->Next()) {
			kernel->ConsumeInput(static_cast<duckdb::idx_t>(accelerator_codegen::Q14InputId::PART),
			                     part_source->Chunk());
		}
		auto lineitem_source = accelerator_codegen::CreateNativeChunkSource(context, *lineitem_scan);
		while (lineitem_source->Next()) {
			kernel->ConsumeInput(static_cast<duckdb::idx_t>(accelerator_codegen::Q14InputId::LINEITEM),
			                     lineitem_source->Chunk());
		}
		kernel->Finalize();
	});
	std::ostringstream out;
	out << "part_map_bytes=" << counters.part_map_memory_bytes << ";part_card=" << counters.part_map_cardinality;
	return out.str();
}

double GeometricMean(const std::vector<double> &values) {
	Require(!values.empty(), "geomean of empty");
	double log_sum = 0.0;
	for (const auto value : values) {
		Require(value > 0.0, "non-positive speedup in geomean");
		log_sum += std::log(value);
	}
	return std::exp(log_sum / static_cast<double>(values.size()));
}

} // namespace

int main(int argc, char **argv) {
	try {
		AcceleratorMode accelerator_mode = AcceleratorMode::Single;
		int thread_count = 1;
		ParseValidatedCli(argc, argv, accelerator_mode, thread_count);

		const int warmup_iterations = GetEnvInt("ACCEL_BENCH_WARMUP", 3);
		const int measured_iterations = GetEnvInt("ACCEL_BENCH_ITERS", 10);
		const int session_id = GetEnvInt("ACCEL_BENCH_SESSION", 1);
		const auto filters = SplitCommaSeparated(GetEnvString("ACCEL_BENCH_FILTER"));
		Require(warmup_iterations >= 3, "ACCEL_BENCH_WARMUP must be >= 3");
		Require(measured_iterations >= 5, "ACCEL_BENCH_ITERS must be >= 5");
		Require(accelerator_codegen::Q1FusedScanEnabled(), "production Q1 path must enable fused scan");
		Require(accelerator_codegen::Q1FusedAlignedPayloadEnabled(),
		        "production Q1 path must enable aligned payload");
		Require(accelerator_codegen::Q1FusedDirectIndexEnabled(),
		        "production Q1 path must enable direct index");
		Require(!accelerator_codegen::Q4LineitemFusedScanEnabled(),
		        "production Q4 path must leave lineitem fused off");
		Require(GetEnvString("DUCKDB_BITPACKING_NATIVE_FILTER").empty() ||
		            GetEnvString("DUCKDB_BITPACKING_NATIVE_FILTER") == "0",
		        "validated Q6 path leaves DUCKDB_BITPACKING_NATIVE_FILTER off");
		Require(GetEnvString("ACCEL_Q1_AUX_STATE").empty() || GetEnvString("ACCEL_Q1_AUX_STATE") == "0",
		        "ACCEL_Q1_AUX_STATE must remain disabled");
#ifdef ACCEL_ENABLE_PARALLEL_TPCH
		const bool parallel_build = true;
#else
		const bool parallel_build = false;
#endif
		if (accelerator_mode == AcceleratorMode::Parallel) {
			Require(parallel_build,
			        "parallel accelerator mode requires a build with -DACCEL_ENABLE_PARALLEL_TPCH=ON");
		}

		PrintEnvBanner();
		std::cerr << "accelerator_mode="
		          << (accelerator_mode == AcceleratorMode::Parallel ? "parallel" : "single")
		          << " threads=" << thread_count << " parallel_build=" << (parallel_build ? "1" : "0") << "\n";

		duckdb::DuckDB database(GetTpchDbPath().c_str());
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		auto pragma = connection.Query("PRAGMA threads=" + std::to_string(thread_count));
		Require(pragma != nullptr && !pragma->HasError(), "failed to set PRAGMA threads");
		const auto threads = VerifyThreads(connection);
		Require(threads == thread_count,
		        "expected current_setting('threads')==" + std::to_string(thread_count) + ", got " +
		            std::to_string(threads));
		std::cerr << "verified threads=" << threads << " session=" << session_id << "\n";

		const auto q1_fn = AccelFunctionName("accel_tpch_q1", accelerator_mode);
		const auto q3_fn = AccelFunctionName("accel_tpch_q3", accelerator_mode);
		const auto q4_fn = AccelFunctionName("accel_tpch_q4", accelerator_mode);
		const auto q6_fn = AccelFunctionName("accel_tpch_q6", accelerator_mode);
		const auto q9_fn = AccelFunctionName("accel_tpch_q9", accelerator_mode);
		const auto q12_fn = AccelFunctionName("accel_tpch_q12", accelerator_mode);
		const auto q14_fn = AccelFunctionName("accel_tpch_q14", accelerator_mode);

		auto lineitem_count = RunQuery(connection, "SELECT count(*) FROM lineitem");
		const auto n_lineitem =
		    lineitem_count->Cast<duckdb::MaterializedQueryResult>().GetValue(0, 0).GetValue<int64_t>();
		const int scale_factor = InferScaleFactor(GetTpchDbPath(), n_lineitem);
		const int64_t expected_lineitems = ExpectedLineitemCount(scale_factor);
		Require(expected_lineitems > 0,
		        "unsupported TPC-H scale (set ACCEL_TPCH_SCALE=1|10); lineitem_count=" + std::to_string(n_lineitem));
		Require(n_lineitem == expected_lineitems, "expected SF" + std::to_string(scale_factor) + " lineitem count " +
		                                               std::to_string(expected_lineitems) + ", got " +
		                                               std::to_string(n_lineitem));
		if (scale_factor == 1) {
			Require(measured_iterations >= 5, "ACCEL_BENCH_ITERS must be >= 5 for SF1");
		}
		std::cerr << "verified scale_factor=" << scale_factor << " lineitem_count=" << n_lineitem
		          << " filter=" << (filters.empty() ? "<all>" : GetEnvString("ACCEL_BENCH_FILTER")) << "\n";

		const std::string mode_label =
		    accelerator_mode == AcceleratorMode::Parallel ? "parallel" : "single";

		std::vector<CsvRow> rows;

		auto record = [&](CsvRow row) {
			std::cout << row.query << ',' << row.seed << ',' << '"' << row.parameters << '"' << ',' << std::fixed
			          << std::setprecision(4) << row.duckdb_median_ms << ',' << row.accelerator_median_ms << ','
			          << row.speedup << ',' << '"' << row.duckdb_dispersion << '"' << ',' << '"'
			          << row.accelerator_dispersion << '"' << ',' << (row.correctness_passed ? "true" : "false") << ','
			          << '"' << row.transient_state << '"' << ',' << row.duckdb_cv << ',' << row.accelerator_cv << ','
			          << row.first_duckdb_ms << ',' << row.first_accelerator_ms << ',' << '"' << row.order_note << '"'
			          << ',' << row.threads << ',' << row.accelerator_mode << ',' << row.shared_state_bytes << ','
			          << row.per_worker_state_bytes << ',' << row.query_local_state_bytes << ',' << row.peak_rss_kb
			          << "\n";
			rows.push_back(std::move(row));
		};

		auto fill_timing = [&](CsvRow &row, const PairedTiming &timing, bool accel_first_even,
		                       const std::string &st_state) {
			row.duckdb_median_ms = timing.duckdb.median_us / 1000.0;
			row.accelerator_median_ms = timing.accelerator.median_us / 1000.0;
			row.speedup = timing.duckdb.median_us / timing.accelerator.median_us;
			row.duckdb_dispersion = FormatDispersion(timing.duckdb);
			row.accelerator_dispersion = FormatDispersion(timing.accelerator);
			row.correctness_passed = true;
			row.duckdb_cv = timing.duckdb.cv;
			row.accelerator_cv = timing.accelerator.cv;
			row.first_duckdb_ms = timing.first_duckdb_us / 1000.0;
			row.first_accelerator_ms = timing.first_accelerator_us / 1000.0;
			row.order_note = accel_first_even ? "even_iters_accel_first" : "even_iters_sql_first";
			row.threads = thread_count;
			row.accelerator_mode = mode_label;
			row.peak_rss_kb = timing.peak_rss_kb;
			row.transient_state =
			    AnnotateMemory(row.query, st_state, thread_count, accelerator_mode == AcceleratorMode::Parallel,
			                   row.shared_state_bytes, row.per_worker_state_bytes, row.query_local_state_bytes);
		};

		std::cout << "query,seed,parameters,duckdb_median_ms,accelerator_median_ms,speedup,duckdb_dispersion,"
		             "accelerator_dispersion,correctness_passed,transient_state,duckdb_cv,accelerator_cv,"
		             "first_duckdb_ms,first_accelerator_ms,order_note,threads,accelerator_mode,shared_state_bytes,"
		             "per_worker_state_bytes,query_local_state_bytes,peak_rss_kb\n";

		// Q1
		for (std::size_t i = 0; i < accelerator_codegen::test_support::kQ1Manifest.size(); i++) {
			const auto &entry = accelerator_codegen::test_support::kQ1Manifest[i];
			if (!MatchesFilter(entry.label, filters)) {
				continue;
			}
			const auto reference_sql = ReadFile(entry.sql_path);
			const auto accel_sql = "SELECT * FROM " + q1_fn + "(" + std::to_string(entry.day_offset) + ")";
			RequireEqualResults(connection, entry.label, reference_sql, accel_sql, false);
			const bool accel_first_even = ((session_id + static_cast<int>(i)) % 2) == 0;
			const auto timing = BenchmarkPaired(connection, reference_sql, accel_sql, warmup_iterations,
			                                    measured_iterations, accel_first_even);
			CsvRow row;
			row.query = "Q1";
			row.seed = entry.label;
			row.parameters = "day_offset=" + std::to_string(entry.day_offset);
			fill_timing(row, timing, accel_first_even, "none");
			record(std::move(row));
		}

		// Q3 — measure ST profile counters AFTER timing (avoids asymmetric ST warmup before parallel timing)
		for (std::size_t i = 0; i < accelerator_codegen::test_support::kQ3Manifest.size(); i++) {
			const auto &entry = accelerator_codegen::test_support::kQ3Manifest[i];
			if (!MatchesFilter(entry.label, filters)) {
				continue;
			}
			const auto reference_sql = ReadFile(entry.sql_path);
			const auto accel_sql = std::string("SELECT * FROM ") + q3_fn + "('" + entry.market_segment + "', DATE '" +
			                       entry.cutoff_date + "')";
			RequireEqualResults(connection, entry.label, reference_sql, accel_sql, true);
			const bool accel_first_even = ((session_id + static_cast<int>(i)) % 2) == 0;
			const auto timing = BenchmarkPaired(connection, reference_sql, accel_sql, warmup_iterations,
			                                    measured_iterations, accel_first_even);
			const auto state = MeasureQ3State(connection, entry);
			CsvRow row;
			row.query = "Q3";
			row.seed = entry.label;
			row.parameters = std::string("segment=") + entry.market_segment + ";cutoff=" + entry.cutoff_date;
			fill_timing(row, timing, accel_first_even, state);
			record(std::move(row));
		}

		// Q4
		for (std::size_t i = 0; i < accelerator_codegen::test_support::kQ4Manifest.size(); i++) {
			const auto &entry = accelerator_codegen::test_support::kQ4Manifest[i];
			if (!MatchesFilter(entry.label, filters)) {
				continue;
			}
			const auto reference_sql = ReadFile(entry.sql_path);
			const auto accel_sql = std::string("SELECT * FROM ") + q4_fn + "(DATE '" + entry.start_date + "')";
			RequireEqualResults(connection, entry.label, reference_sql, accel_sql, false);
			const bool accel_first_even = ((session_id + static_cast<int>(i)) % 2) == 0;
			const auto timing = BenchmarkPaired(connection, reference_sql, accel_sql, warmup_iterations,
			                                    measured_iterations, accel_first_even);
			const auto state = MeasureQ4State(connection, entry);
			CsvRow row;
			row.query = "Q4";
			row.seed = entry.label;
			row.parameters = std::string("start_date=") + entry.start_date;
			fill_timing(row, timing, accel_first_even, state);
			record(std::move(row));
		}

		// Q6
		for (std::size_t i = 0; i < accelerator_codegen::test_support::kQ6Manifest.size(); i++) {
			const auto &entry = accelerator_codegen::test_support::kQ6Manifest[i];
			if (!MatchesFilter(entry.label, filters)) {
				continue;
			}
			const auto reference_sql = ReadFile(entry.sql_path);
			const auto accel_sql = std::string("SELECT revenue FROM ") + q6_fn + "(DATE '" + entry.start_date + "', " +
			                       entry.discount + ", " + entry.quantity + ")";
			RequireEqualResults(connection, entry.label, reference_sql, accel_sql, false);
			const bool accel_first_even = ((session_id + static_cast<int>(i)) % 2) == 0;
			const auto timing = BenchmarkPaired(connection, reference_sql, accel_sql, warmup_iterations,
			                                    measured_iterations, accel_first_even);
			CsvRow row;
			row.query = "Q6";
			row.seed = entry.label;
			row.parameters = std::string("start=") + entry.start_date + ";discount=" + entry.discount +
			                ";quantity=" + entry.quantity;
			fill_timing(row, timing, accel_first_even, "none");
			record(std::move(row));
		}

		// Q9
		for (std::size_t i = 0; i < accelerator_codegen::test_support::kQ9Manifest.size(); i++) {
			const auto &entry = accelerator_codegen::test_support::kQ9Manifest[i];
			if (!MatchesFilter(entry.label, filters)) {
				continue;
			}
			const auto reference_sql = ReadFile(entry.sql_path);
			const auto accel_sql =
			    std::string("SELECT * FROM ") + q9_fn + "('" + EscapeSqlLiteral(entry.color) + "')";
			RequireEqualResults(connection, entry.label, reference_sql, accel_sql, false);
			const bool accel_first_even = ((session_id + static_cast<int>(i)) % 2) == 0;
			const auto timing = BenchmarkPaired(connection, reference_sql, accel_sql, warmup_iterations,
			                                    measured_iterations, accel_first_even);
			const auto state = MeasureQ9State(connection, entry);
			CsvRow row;
			row.query = "Q9";
			row.seed = entry.label;
			row.parameters = std::string("color=") + entry.color;
			fill_timing(row, timing, accel_first_even, state);
			record(std::move(row));
		}

		// Q12
		for (std::size_t i = 0; i < accelerator_codegen::test_support::kQ12Manifest.size(); i++) {
			const auto &entry = accelerator_codegen::test_support::kQ12Manifest[i];
			if (!MatchesFilter(entry.label, filters)) {
				continue;
			}
			const auto reference_sql = ReadFile(entry.sql_path);
			const auto accel_sql = std::string("SELECT * FROM ") + q12_fn + "('" +
			                       EscapeSqlLiteral(entry.shipmode1) + "', '" + EscapeSqlLiteral(entry.shipmode2) +
			                       "', DATE '" + entry.start_date + "')";
			RequireEqualResults(connection, entry.label, reference_sql, accel_sql, false);
			const bool accel_first_even = ((session_id + static_cast<int>(i)) % 2) == 0;
			const auto timing = BenchmarkPaired(connection, reference_sql, accel_sql, warmup_iterations,
			                                    measured_iterations, accel_first_even);
			const auto state = MeasureQ12State(connection, entry);
			CsvRow row;
			row.query = "Q12";
			row.seed = entry.label;
			row.parameters = std::string("shipmode1=") + entry.shipmode1 + ";shipmode2=" + entry.shipmode2 +
			                ";start=" + entry.start_date;
			fill_timing(row, timing, accel_first_even, state);
			record(std::move(row));
		}

		// Q14
		for (std::size_t i = 0; i < accelerator_codegen::test_support::kQ14Manifest.size(); i++) {
			const auto &entry = accelerator_codegen::test_support::kQ14Manifest[i];
			if (!MatchesFilter(entry.label, filters)) {
				continue;
			}
			const auto reference_sql = ReadFile(entry.sql_path);
			const auto accel_sql = std::string("SELECT * FROM ") + q14_fn + "(DATE '" + entry.start_date + "')";
			RequireEqualResults(connection, entry.label, reference_sql, accel_sql, false);
			const bool accel_first_even = ((session_id + static_cast<int>(i)) % 2) == 0;
			const auto timing = BenchmarkPaired(connection, reference_sql, accel_sql, warmup_iterations,
			                                    measured_iterations, accel_first_even);
			const auto state = MeasureQ14State(connection, entry);
			CsvRow row;
			row.query = "Q14";
			row.seed = entry.label;
			row.parameters = std::string("start_date=") + entry.start_date;
			fill_timing(row, timing, accel_first_even, state);
			record(std::move(row));
		}

		std::cerr << "\n=== per-query speedup summary (median-of-medians path times; arithmetic/geomean of per-seed "
		             "speedups) ===\n";
		const char *queries[] = {"Q1", "Q3", "Q4", "Q6", "Q9", "Q12", "Q14"};
		for (const auto &query : queries) {
			std::vector<double> speedups;
			std::vector<double> duckdb_ms;
			std::vector<double> accel_ms;
			double max_cv = 0.0;
			for (const auto &row : rows) {
				if (row.query != query) {
					continue;
				}
				speedups.push_back(row.speedup);
				duckdb_ms.push_back(row.duckdb_median_ms);
				accel_ms.push_back(row.accelerator_median_ms);
				max_cv = std::max(max_cv, std::max(row.duckdb_cv, row.accelerator_cv));
			}
			if (speedups.empty()) {
				std::cerr << query << ": seeds=0 (filtered out)\n";
				continue;
			}
			const double arith = std::accumulate(speedups.begin(), speedups.end(), 0.0) / speedups.size();
			const double geo = GeometricMean(speedups);
			const double duck_med = MedianOf(duckdb_ms);
			const double accel_med = MedianOf(accel_ms);
			const double ratio_of_medians = duck_med / accel_med;
			std::cerr << query << ": seeds=" << speedups.size() << " arith_mean_speedup=" << std::fixed
			          << std::setprecision(4) << arith << " geomean_speedup=" << geo
			          << " ratio_of_median_ms=" << ratio_of_medians << " duckdb_median_ms=" << duck_med
			          << " accel_median_ms=" << accel_med << " speedup_min=" << *std::min_element(speedups.begin(), speedups.end())
			          << " speedup_max=" << *std::max_element(speedups.begin(), speedups.end())
			          << " max_cv=" << max_cv;
			if (max_cv > 0.15) {
				std::cerr << " VARIANCE_FLAG";
			}
			std::cerr << "\n";
		}

		std::vector<double> query_geomeans;
		for (const auto &query : queries) {
			std::vector<double> speedups;
			for (const auto &row : rows) {
				if (row.query == query) {
					speedups.push_back(row.speedup);
				}
			}
			if (!speedups.empty()) {
				query_geomeans.push_back(GeometricMean(speedups));
			}
		}
		Require(!query_geomeans.empty(), "no benchmark rows matched filter");
		std::cerr << "overall_geomean_across_all_queries=" << std::fixed << std::setprecision(4)
		          << GeometricMean(query_geomeans) << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_tpch_validated failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
