#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q14_aux.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "duckdb.hpp"

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

std::string GetEnv(const char *name, const std::string &fallback) {
	const char *value = std::getenv(name);
	return value ? value : fallback;
}

int GetEnvInt(const char *name, int fallback) {
	const char *value = std::getenv(name);
	return value ? std::stoi(value) : fallback;
}

void ConfigureConnection(duckdb::Connection &connection, const std::string &mode) {
	if (mode == "isolation") {
		connection.Query("PRAGMA threads=1");
	}
}

void BuildAuxState(duckdb::Connection &connection, int query_num) {
	switch (query_num) {
	case 1:
		connection.Query("SELECT * FROM accel_q1_aux_build()");
		break;
	case 3:
		connection.Query("SELECT * FROM accel_q3_aux_build()");
		break;
	case 10:
		connection.Query("SELECT * FROM accel_q10_aux_build()");
		break;
	case 12:
		connection.Query("SELECT * FROM accel_q12_aux_build()");
		break;
	case 14:
		connection.Query("SELECT * FROM accel_q14_aux_build()");
		break;
	default:
		throw std::runtime_error("unsupported query number");
	}
}

bool RunAuxQuery(duckdb::Connection &connection, int query_num) {
	std::string sql;
	switch (query_num) {
	case 1:
		sql = "SELECT * FROM accel_tpch_q1_aux(90)";
		break;
	case 3:
		sql = "SELECT * FROM accel_tpch_q3_aux('BUILDING', DATE '1995-03-15')";
		break;
	case 10:
		sql = "SELECT * FROM accel_tpch_q10_aux(DATE '1993-06-01')";
		break;
	case 12:
		sql = "SELECT * FROM accel_tpch_q12_aux('MAIL', 'SHIP', DATE '1995-01-01')";
		break;
	case 14:
		sql = "SELECT * FROM accel_tpch_q14_aux(DATE '1995-06-01')";
		break;
	default:
		return false;
	}
	auto result = connection.Query(sql);
	return result && !result->HasError();
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

} // namespace

int main(int argc, char **argv) {
	const char *db_path = argc > 1 ? argv[1] : "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
	const int query_num = argc > 2 ? std::stoi(argv[2]) : 14;
	const std::string mode = argc > 3 ? argv[3] : GetEnv("ACCEL_MULTI_CONN_MODE", "isolation");
	const int readers = GetEnvInt("ACCEL_MULTI_CONN_READERS", 4);
	const int duration_sec = GetEnvInt("ACCEL_MULTI_CONN_DURATION_SEC", 30);

	try {
		duckdb::DuckDB database(db_path);
		duckdb::Connection setup(database);
		accelerator_codegen::RegisterAcceleratorFunctions(setup);
		ConfigureConnection(setup, mode);
		BuildAuxState(setup, query_num);

		std::atomic<bool> stop {false};
		std::vector<double> latencies;
		std::mutex latency_mutex;
		std::atomic<uint64_t> successes {0};
		std::atomic<uint64_t> failures {0};
		std::atomic<uint64_t> writer_commits {0};

		const auto benchmark_start = Clock::now();
		const auto deadline = benchmark_start + std::chrono::seconds(duration_sec);

		std::thread writer([&]() {
			duckdb::Connection connection(database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			ConfigureConnection(connection, mode);
			while (Clock::now() < deadline) {
				auto result = connection.Query(
				    "UPDATE lineitem SET l_comment = l_comment WHERE l_orderkey % 10007 = 0");
				if (result && !result->HasError()) {
					writer_commits.fetch_add(1);
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
			}
		});

		StartBarrier start_barrier(readers + 1);
		std::vector<std::thread> threads;
		for (int reader = 0; reader < readers; reader++) {
			threads.emplace_back([&]() {
				duckdb::Connection connection(database);
				accelerator_codegen::RegisterAcceleratorFunctions(connection);
				ConfigureConnection(connection, mode);
				start_barrier.ArriveAndWait();
				while (Clock::now() < deadline) {
					const auto start = Clock::now();
					if (RunAuxQuery(connection, query_num)) {
						successes.fetch_add(1);
					} else {
						failures.fetch_add(1);
					}
					const auto us = ElapsedUs(start, Clock::now());
					std::lock_guard<std::mutex> lock(latency_mutex);
					latencies.push_back(us);
				}
			});
		}
		start_barrier.ArriveAndWait();
		for (auto &thread : threads) {
			thread.join();
		}
		writer.join();

		const auto elapsed_sec =
		    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - benchmark_start).count() / 1000.0;
		const auto total_queries = successes.load() + failures.load();
		const auto throughput = elapsed_sec > 0.0 ? static_cast<double>(total_queries) / elapsed_sec : 0.0;
		const auto hit_rate = total_queries > 0 ? static_cast<double>(successes.load()) / static_cast<double>(total_queries) : 0.0;

		std::cout << "benchmark_mode,query_num,readers,duration_sec,throughput_qps,p50_query_us,p95_query_us,hit_rate,"
		             "writer_commits,samples\n";
		std::cout << mode << ',' << query_num << ',' << readers << ',' << duration_sec << ',' << throughput << ','
		          << Percentile(latencies, 0.50) << ',' << Percentile(latencies, 0.95) << ',' << hit_rate << ','
		          << writer_commits.load() << ',' << latencies.size() << '\n';
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_aux_multi_connection failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
