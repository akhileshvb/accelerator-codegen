#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/sec_edgar_q3_aux.hpp"
#include "accelerator_codegen/sec_edgar_q3_maintenance.hpp"
#include "test_support/interference_calibration.hpp"
#include "test_support/sec_edgar_q3_aux_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "duckdb.hpp"

namespace {
using Clock = std::chrono::steady_clock;

double ElapsedUs(const Clock::time_point &start, const Clock::time_point &end) {
	return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

double MedianUs(std::vector<double> samples) {
	if (samples.empty()) {
		return 0.0;
	}
	std::sort(samples.begin(), samples.end());
	return samples[samples.size() / 2];
}

double TimeQueryUs(duckdb::Connection &connection, const std::string &sql) {
	const auto start = Clock::now();
	auto result = connection.Query(sql);
	if (!result || result->HasError()) {
		throw std::runtime_error("query failed: " + sql + " error=" + (result ? result->GetError() : "null"));
	}
	return ElapsedUs(start, Clock::now());
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
	int arrived = 0;
	std::mutex mutex;
	std::condition_variable cv;
};

void ConfigureConnection(duckdb::Connection &connection) {
	connection.Query("PRAGMA threads=1");
}

accelerator_codegen::test_support::InterferenceCalibration CalibrateSecEdgarQ3Interference(
    duckdb::DuckDB &database) {
	const int target_readers = GetEnvInt("ACCEL_MULTI_CONN_TARGET_READERS", 3);
	const int background_readers = GetEnvInt("ACCEL_MULTI_CONN_BACKGROUND_READERS", 2);
	const int duration_sec = GetEnvInt("ACCEL_INTERFERENCE_CALIB_SEC", 4);
	const int isolated_samples = GetEnvInt("ACCEL_INTERFERENCE_ISOLATED_SAMPLES", 25);

	accelerator_codegen::test_support::InterferenceCalibration calibration;
	calibration.target_reader_count = target_readers;
	calibration.background_reader_count = background_readers;

	const auto aux_sql = accelerator_codegen::test_support::AcceleratorSecEdgarQ3AuxSql();
	const auto base_sql = accelerator_codegen::test_support::AcceleratorSecEdgarQ3Sql();

	{
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		ConfigureConnection(connection);
		std::vector<double> latencies;
		latencies.reserve(isolated_samples);
		for (int i = 0; i < isolated_samples; i++) {
			const auto start = Clock::now();
			TimeQueryUs(connection, aux_sql);
			latencies.push_back(ElapsedUs(start, Clock::now()));
		}
		calibration.isolated_query_p50_us = MedianUs(latencies);
	}

	{
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		ConfigureConnection(connection);
		const auto start = Clock::now();
		TimeQueryUs(connection, "SELECT * FROM accel_sec_edgar_q3_aux_rebuild_partition(0::BIGINT)");
		calibration.isolated_rebuild_us = ElapsedUs(start, Clock::now());
	}

	std::vector<double> target_latencies;
	std::mutex latency_mutex;
	std::atomic<bool> stop {false};
	const auto deadline = Clock::now() + std::chrono::seconds(duration_sec);
	StartBarrier start_barrier(target_readers + background_readers + 1);
	std::vector<std::thread> threads;

	for (int i = 0; i < target_readers; i++) {
		threads.emplace_back([&]() {
			duckdb::Connection connection(database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			ConfigureConnection(connection);
			start_barrier.ArriveAndWait();
			while (Clock::now() < deadline) {
				const auto start = Clock::now();
				auto result = connection.Query(aux_sql);
				if (!result || result->HasError()) {
					continue;
				}
				std::lock_guard<std::mutex> lock(latency_mutex);
				target_latencies.push_back(ElapsedUs(start, Clock::now()));
			}
		});
	}

	for (int i = 0; i < background_readers; i++) {
		threads.emplace_back([&]() {
			duckdb::Connection connection(database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			ConfigureConnection(connection);
			start_barrier.ArriveAndWait();
			while (Clock::now() < deadline) {
				(void)connection.Query(base_sql);
			}
		});
	}

	std::thread writer([&]() {
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		ConfigureConnection(connection);
		start_barrier.ArriveAndWait();
		int counter = 0;
		while (Clock::now() < deadline) {
			connection.BeginTransaction();
			connection.Query(
			    "INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, coreg, value, footnote) "
			    "SELECT n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, "
			    "n.value + 0.001 * " +
			    std::to_string(counter) +
			    ", n.footnote FROM num n JOIN sub s ON n.adsh = s.adsh "
			    "WHERE n.uom = 'pure' AND s.fy = 2022 AND n.value IS NOT NULL LIMIT 1");
			connection.Commit();
			counter++;
			if (counter > 200) {
				break;
			}
		}
	});

	start_barrier.ArriveAndWait();
	for (auto &thread : threads) {
		thread.join();
	}
	writer.join();

	calibration.concurrent_query_p50_us = MedianUs(target_latencies);

	{
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		ConfigureConnection(connection);
		std::atomic<bool> rebuild_stop {false};
		const auto rebuild_deadline = Clock::now() + std::chrono::seconds(duration_sec);
		std::vector<std::thread> readers;
		StartBarrier rebuild_barrier(target_readers + 1);
		for (int i = 0; i < target_readers; i++) {
			readers.emplace_back([&]() {
				duckdb::Connection reader_conn(database);
				accelerator_codegen::RegisterAcceleratorFunctions(reader_conn);
				ConfigureConnection(reader_conn);
				rebuild_barrier.ArriveAndWait();
				while (Clock::now() < rebuild_deadline) {
					(void)reader_conn.Query(aux_sql);
				}
			});
		}
		rebuild_barrier.ArriveAndWait();
		const auto start = Clock::now();
		TimeQueryUs(connection, "SELECT * FROM accel_sec_edgar_q3_aux_rebuild_partition(0::BIGINT)");
		calibration.concurrent_rebuild_us = ElapsedUs(start, Clock::now());
		rebuild_stop.store(true);
		for (auto &reader : readers) {
			reader.join();
		}
	}

	const auto overlapping = std::max(1, target_readers + background_readers - 1);
	const auto query_delta = std::max(0.0, calibration.concurrent_query_p50_us - calibration.isolated_query_p50_us);
	calibration.interference_delta_per_query_us = std::max(0.5, query_delta / static_cast<double>(overlapping));
	calibration.interference_invalidate_per_query_us = calibration.interference_delta_per_query_us * 0.25;
	const auto rebuild_delta = std::max(0.0, calibration.concurrent_rebuild_us - calibration.isolated_rebuild_us);
	calibration.interference_rebuild_per_query_us =
	    std::max(1.0, rebuild_delta / static_cast<double>(target_readers));
	calibration.interference_rebuild_scan_byte_us = 0.0;
	if (calibration.isolated_query_p50_us > 0.0) {
		calibration.interference_query_slowdown_factor =
		    std::max(0.0, (calibration.concurrent_query_p50_us / calibration.isolated_query_p50_us - 1.0) /
		                      static_cast<double>(overlapping));
	}
	return calibration;
}

void WriteCostModel(const std::string &out_path,
                    const accelerator_codegen::test_support::InterferenceCalibration &interference, double aux_hit_us,
                    double fallback_us, double partition_rebuild_us, double full_rebuild_us, double first_query_blocking_us) {
	std::ofstream out(out_path);
	out << "{\n"
	    << "  \"version\": 1,\n"
	    << "  \"aux_hit_us\": " << aux_hit_us << ",\n"
	    << "  \"fallback_us\": " << fallback_us << ",\n"
	    << "  \"partition_rebuild_us\": " << partition_rebuild_us << ",\n"
	    << "  \"full_rebuild_us\": " << full_rebuild_us << ",\n"
	    << "  \"first_query_blocking_us\": " << first_query_blocking_us << ",\n"
	    << "  \"apply_delta_overhead_us\": 100.0,\n"
	    << "  \"apply_delta_base_us\": 0.0,\n"
	    << "  \"apply_delta_per_row_us\": 0.5,\n"
	    << "  \"apply_delta_max_rows\": 1024,\n"
	    << "  \"apply_delta_max_part_fanout\": 16,\n"
	    << "  \"invalidation_overhead_us\": 10.0,\n"
	    << "  \"decision_margin_us\": 1000.0,\n"
	    << "  \"min_samples\": 8,\n"
	    << "  \"incremental_failure_prob\": 0.01,\n"
	    << "  \"interference_rebuild_per_query_us\": " << interference.interference_rebuild_per_query_us << ",\n"
	    << "  \"interference_rebuild_scan_byte_us\": " << interference.interference_rebuild_scan_byte_us << ",\n"
	    << "  \"interference_delta_per_query_us\": " << interference.interference_delta_per_query_us << ",\n"
	    << "  \"interference_invalidate_per_query_us\": " << interference.interference_invalidate_per_query_us << ",\n"
	    << "  \"interference_query_slowdown_factor\": " << interference.interference_query_slowdown_factor << "\n"
	    << "}\n";
}

} // namespace

int main(int argc, char **argv) {
	const char *db_path = argc > 1 ? argv[1] : "/home/akhilvb/accelerator-codegen-duckdb/sec_edgar_sf3.duckdb";
	const char *out_path = argc > 2 ? argv[2] : "config/sec_edgar_q3_aux_cost_model.json";
	try {
		duckdb::DuckDB database(db_path);
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);
		ConfigureConnection(connection);
		connection.Query("SELECT * FROM accel_sec_edgar_q3_aux_build()");

		const auto aux_sql = accelerator_codegen::test_support::AcceleratorSecEdgarQ3AuxSql();
		std::vector<double> aux_hits;
		std::vector<double> fallbacks;
		for (int i = 0; i < 50; i++) {
			aux_hits.push_back(TimeQueryUs(connection, aux_sql));
		}

		const auto partition_rebuild_start = Clock::now();
		TimeQueryUs(connection, "SELECT * FROM accel_sec_edgar_q3_aux_rebuild_partition(0::BIGINT)");
		const auto partition_rebuild_us = ElapsedUs(partition_rebuild_start, Clock::now());

		accelerator_codegen::InvalidateSecEdgarQ3AuxStateForTests(*connection.context);
		for (int i = 0; i < 50; i++) {
			fallbacks.push_back(TimeQueryUs(connection, aux_sql));
		}

		connection.Query("SELECT * FROM accel_sec_edgar_q3_aux_build()");
		const auto first_query_start = Clock::now();
		TimeQueryUs(connection, aux_sql);
		const auto first_query_total_us = ElapsedUs(first_query_start, Clock::now());
		const auto aux_hit_median = MedianUs(aux_hits);
		const auto first_query_blocking_us =
		    std::max(0.0, first_query_total_us - partition_rebuild_us - aux_hit_median);

		const auto full_rebuild_start = Clock::now();
		connection.Query("SELECT * FROM accel_sec_edgar_q3_aux_build()");
		const auto full_rebuild_us = ElapsedUs(full_rebuild_start, Clock::now());

		const auto interference = CalibrateSecEdgarQ3Interference(database);
		WriteCostModel(out_path, interference, aux_hit_median, MedianUs(fallbacks), partition_rebuild_us,
		               full_rebuild_us, first_query_blocking_us);

		std::cout << "calibrate_sec_edgar_q3_aux_cost_model wrote " << out_path << '\n';
		std::cout << "interference_delta_per_query_us=" << interference.interference_delta_per_query_us << '\n';
		std::cout << "interference_rebuild_per_query_us=" << interference.interference_rebuild_per_query_us << '\n';
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "calibrate_sec_edgar_q3_aux_cost_model failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
