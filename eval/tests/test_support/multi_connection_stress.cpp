#include "backends/duckdb/api/shared_query_engine.hpp"
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "eval/tests/test_support/interference_calibration.hpp"
#include "eval/tests/test_support/cross_accel_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "duckdb.hpp"

namespace accelerator_codegen::test_support {
namespace {

using Clock = std::chrono::steady_clock;

double ElapsedUs(const Clock::time_point &start, const Clock::time_point &end) {
	return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

double Percentile(std::vector<double> values, double percentile) {
	if (values.empty()) {
		return 0.0;
	}
	std::sort(values.begin(), values.end());
	const auto index = static_cast<size_t>(percentile * static_cast<double>(values.size() - 1));
	return values[index];
}

int GetEnvInt(const char *name, int fallback) {
	const char *value = std::getenv(name);
	return value ? std::stoi(value) : fallback;
}

struct StartBarrier {
	explicit StartBarrier(int participants) : threshold(participants), arrived(0) {
	}

	void ArriveAndWait() {
		std::unique_lock<std::mutex> lock(mutex);
		if (++arrived == threshold) {
			cv.notify_all();
			return;
		}
		cv.wait(lock, [&] { return arrived >= threshold; });
	}

private:
	const int threshold;
	int arrived;
	std::mutex mutex;
	std::condition_variable cv;
};

void ConfigureConnection(duckdb::Connection &connection) {
	connection.Query("PRAGMA threads=1");
}

bool RunAuxQuery(duckdb::Connection &connection, int query_num) {
	auto result = connection.Query(CrossAccelAuxSql(query_num));
	return result && !result->HasError();
}

std::vector<int> OtherQueries(const std::vector<int> &all_queries, int target_query) {
	std::vector<int> others;
	for (int query : all_queries) {
		if (query != target_query) {
			others.push_back(query);
		}
	}
	return others;
}

void BuildAllAux(duckdb::Connection &connection, const std::vector<int> &queries) {
	for (int query_num : queries) {
		BuildCrossAccelAux(connection, query_num);
	}
}

double MeasureIsolatedQueryP50(duckdb::DuckDB &database, int target_query, int samples) {
	duckdb::Connection connection(database);
	RegisterAcceleratorFunctions(connection);
	ConfigureConnection(connection);
	std::vector<double> latencies;
	latencies.reserve(samples);
	for (int i = 0; i < samples; i++) {
		const auto start = Clock::now();
		if (!RunAuxQuery(connection, target_query)) {
			throw std::runtime_error("isolated aux query failed for Q" + std::to_string(target_query));
		}
		latencies.push_back(ElapsedUs(start, Clock::now()));
	}
	return Percentile(latencies, 0.50);
}

struct ConcurrentStressResult {
	std::vector<double> target_latencies_us;
	uint64_t target_failures = 0;
	uint64_t writer_commits = 0;
};

ConcurrentStressResult RunConcurrentReadersAndWriters(duckdb::DuckDB &database, int target_query,
                                                      const std::vector<int> &all_queries, int duration_sec,
                                                      int target_readers, int background_readers) {
	const auto others = OtherQueries(all_queries, target_query);
	if (others.empty() && background_readers > 0) {
		background_readers = 0;
	}
	if (others.empty() && target_readers == 0) {
		throw std::runtime_error("multi-connection stress requires at least one target reader");
	}

	ConcurrentStressResult result;
	std::atomic<bool> stop {false};
	const auto deadline = Clock::now() + std::chrono::seconds(duration_sec);
	std::mutex latency_mutex;

	const int participants = target_readers + background_readers + 1;
	StartBarrier start_barrier(participants);
	std::vector<std::thread> threads;

	for (int i = 0; i < target_readers; i++) {
		threads.emplace_back([&]() {
			duckdb::Connection connection(database);
			RegisterAcceleratorFunctions(connection);
			ConfigureConnection(connection);
			start_barrier.ArriveAndWait();
			while (!stop.load() && Clock::now() < deadline) {
				const auto start = Clock::now();
				if (RunAuxQuery(connection, target_query)) {
					std::lock_guard<std::mutex> lock(latency_mutex);
					result.target_latencies_us.push_back(ElapsedUs(start, Clock::now()));
				} else {
					result.target_failures++;
				}
			}
		});
	}

	for (int i = 0; i < background_readers; i++) {
		threads.emplace_back([&, i]() {
			duckdb::Connection connection(database);
			RegisterAcceleratorFunctions(connection);
			ConfigureConnection(connection);
			start_barrier.ArriveAndWait();
			size_t cursor = static_cast<size_t>(i) % others.size();
			while (!stop.load() && Clock::now() < deadline) {
				const auto bg_query = others[cursor % others.size()];
				cursor++;
				if (!RunAuxQuery(connection, bg_query)) {
					result.target_failures++;
				}
			}
		});
	}

	start_barrier.ArriveAndWait();

	std::thread writer([&]() {
		duckdb::Connection connection(database);
		RegisterAcceleratorFunctions(connection);
		ConfigureConnection(connection);
		std::mt19937 rng(17);
		std::uniform_int_distribution<size_t> pick(0, all_queries.size() - 1);
		while (Clock::now() < deadline) {
			const auto writer_query = all_queries[pick(rng)];
			try {
				ApplyMultiConnectionStressWriter(connection, writer_query);
				result.writer_commits++;
			} catch (...) {
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		stop.store(true);
	});

	for (auto &thread : threads) {
		thread.join();
	}
	writer.join();
	return result;
}

double RebuildTargetPartitionUs(duckdb::Connection &connection, int target_query) {
	using namespace accelerator_codegen::aux_state::engine;
	using namespace accelerator_codegen::aux_state::engine::duckdb;
	const auto benchmark = DetectCrossAccelBenchmark();
	const auto workload =
	    benchmark == CrossAccelBenchmark::SecEdgar ? SharedWorkloadKind::SecEdgar
	                                               : SharedWorkloadKind::Tpch;
	const char *workload_name =
	    benchmark == CrossAccelBenchmark::SecEdgar ? "sec_edgar" : "tpch";
	if (!SharedQueryEngine::HasSharedQuery(workload_name, target_query)) {
		throw std::runtime_error("unsupported rebuild for Q" + std::to_string(target_query));
	}
	const auto &entry = LookupSharedQuery(workload, target_query);
	const auto start = Clock::now();
	DuckdbRuntime runtime(connection, entry, 10.0);
	const auto probe = FindSharedProbe(connection, entry);
	runtime.Build();
	runtime.ApplyMutationBatch(BuildSharedMutation(entry, probe));
	return ElapsedUs(start, Clock::now());
}

} // namespace

std::vector<int> MultiConnectionTestQueries() {
	const char *raw = std::getenv("ACCEL_MULTI_CONN_TEST_QUERIES");
	if (raw == nullptr || *raw == '\0') {
		return CrossAccelTestQueries();
	}
	std::vector<int> queries;
	std::stringstream stream(raw);
	std::string token;
	while (std::getline(stream, token, ',')) {
		if (!token.empty()) {
			queries.push_back(std::stoi(token));
		}
	}
	if (queries.empty()) {
		throw std::runtime_error("ACCEL_MULTI_CONN_TEST_QUERIES is empty");
	}
	return queries;
}

void RunMultiConnectionStress(duckdb::DuckDB &database, int target_query, const std::vector<int> &all_queries,
                              int duration_sec) {
	auto queries = all_queries;
	if (queries.empty()) {
		queries = MultiConnectionTestQueries();
		if (std::find(queries.begin(), queries.end(), target_query) == queries.end()) {
			queries.push_back(target_query);
		}
	}
	{
		duckdb::Connection setup(database);
		RegisterAcceleratorFunctions(setup);
		ConfigureConnection(setup);
		BuildAllAux(setup, queries);
	}

	const int target_readers = GetEnvInt("ACCEL_MULTI_CONN_TARGET_READERS", 3);
	const int background_readers = GetEnvInt("ACCEL_MULTI_CONN_BACKGROUND_READERS", 2);
	const auto stress = RunConcurrentReadersAndWriters(database, target_query, queries, duration_sec, target_readers,
	                                                   background_readers);

	if (stress.target_latencies_us.empty()) {
		throw std::runtime_error("multi-connection stress produced no successful target-query samples");
	}
	if (stress.target_failures > 0) {
		throw std::runtime_error("multi-connection stress had target/background query failures: " +
		                         std::to_string(stress.target_failures));
	}

	duckdb::Connection verify(database);
	RegisterAcceleratorFunctions(verify);
	ConfigureConnection(verify);
	RequireCrossAccelQueryCorrectness(verify, target_query, "post_stress_Q" + std::to_string(target_query));
}

InterferenceCalibration CalibrateInterference(duckdb::DuckDB &database, int target_query,
                                              const std::vector<int> &all_queries) {
	auto queries = all_queries;
	if (queries.empty()) {
		queries = MultiConnectionTestQueries();
		if (std::find(queries.begin(), queries.end(), target_query) == queries.end()) {
			queries.push_back(target_query);
		}
	}
	{
		duckdb::Connection setup(database);
		RegisterAcceleratorFunctions(setup);
		ConfigureConnection(setup);
		BuildAllAux(setup, queries);
	}

	InterferenceCalibration calibration;
	calibration.target_reader_count = GetEnvInt("ACCEL_MULTI_CONN_TARGET_READERS", 3);
	calibration.background_reader_count = GetEnvInt("ACCEL_MULTI_CONN_BACKGROUND_READERS", 2);
	const int duration_sec = GetEnvInt("ACCEL_INTERFERENCE_CALIB_SEC", 4);
	const int isolated_samples = GetEnvInt("ACCEL_INTERFERENCE_ISOLATED_SAMPLES", 25);

	calibration.isolated_query_p50_us = MeasureIsolatedQueryP50(database, target_query, isolated_samples);

	{
		duckdb::Connection connection(database);
		RegisterAcceleratorFunctions(connection);
		ConfigureConnection(connection);
		calibration.isolated_rebuild_us = RebuildTargetPartitionUs(connection, target_query);
	}

	const auto stress = RunConcurrentReadersAndWriters(database, target_query, queries, duration_sec,
	                                                   calibration.target_reader_count, calibration.background_reader_count);
	calibration.concurrent_query_p50_us = Percentile(stress.target_latencies_us, 0.50);

	std::atomic<bool> stop {false};
	const auto rebuild_deadline = Clock::now() + std::chrono::seconds(duration_sec);
	std::vector<std::thread> readers;
	StartBarrier barrier(calibration.target_reader_count + 1);
	for (int i = 0; i < calibration.target_reader_count; i++) {
		readers.emplace_back([&]() {
			duckdb::Connection connection(database);
			RegisterAcceleratorFunctions(connection);
			ConfigureConnection(connection);
			barrier.ArriveAndWait();
			while (Clock::now() < rebuild_deadline) {
				RunAuxQuery(connection, target_query);
			}
		});
	}
	barrier.ArriveAndWait();
	{
		duckdb::Connection connection(database);
		RegisterAcceleratorFunctions(connection);
		ConfigureConnection(connection);
		const auto start = Clock::now();
		RebuildTargetPartitionUs(connection, target_query);
		calibration.concurrent_rebuild_us = ElapsedUs(start, Clock::now());
	}
	stop.store(true);
	for (auto &reader : readers) {
		reader.join();
	}

	const auto overlapping = std::max(
	    1, calibration.target_reader_count + calibration.background_reader_count - 1);
	const auto query_delta = std::max(0.0, calibration.concurrent_query_p50_us - calibration.isolated_query_p50_us);
	calibration.interference_delta_per_query_us = std::max(0.5, query_delta / static_cast<double>(overlapping));
	calibration.interference_invalidate_per_query_us = calibration.interference_delta_per_query_us * 0.25;
	const auto rebuild_delta = std::max(0.0, calibration.concurrent_rebuild_us - calibration.isolated_rebuild_us);
	calibration.interference_rebuild_per_query_us =
	    std::max(1.0, rebuild_delta / static_cast<double>(calibration.target_reader_count));
	calibration.interference_rebuild_scan_byte_us = 0.0;
	if (calibration.isolated_query_p50_us > 0.0) {
		calibration.interference_query_slowdown_factor =
		    std::max(0.0, (calibration.concurrent_query_p50_us / calibration.isolated_query_p50_us - 1.0) /
		                      static_cast<double>(overlapping));
	}
	return calibration;
}

} // namespace accelerator_codegen::test_support
