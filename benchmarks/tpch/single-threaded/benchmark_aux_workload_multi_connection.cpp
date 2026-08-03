#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q1_aux.hpp"
#include "accelerator_codegen/tpch_q3_aux.hpp"
#include "accelerator_codegen/tpch_q6_aux.hpp"
#include "accelerator_codegen/tpch_q10_aux.hpp"
#include "accelerator_codegen/tpch_q12_aux.hpp"
#include "accelerator_codegen/tpch_q14_aux.hpp"
#include "accelerator_codegen/sec_edgar_q1_aux.hpp"
#include "accelerator_codegen/sec_edgar_q2_aux.hpp"
#include "accelerator_codegen/sec_edgar_q3_aux.hpp"
#include "accelerator_codegen/sec_edgar_q4_aux.hpp"
#include "accelerator_codegen/sec_edgar_q6_aux.hpp"
#include "accelerator_codegen/sec_edgar_q24_aux.hpp"
#include "test_support/cross_accel_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using CrossAccelBenchmark = accelerator_codegen::test_support::CrossAccelBenchmark;

enum class ReaderKind { Baseline, Accel };

struct PhaseStats {
	std::mutex latencies_mutex;
	std::vector<double> latencies;
	std::atomic<uint64_t> successes {0};
	std::atomic<uint64_t> failures {0};
	std::atomic<uint64_t> aux_hits {0};
	std::atomic<uint64_t> aux_reads {0};
	uint64_t writer_commits = 0;
	double writer_total_us = 0.0;
};

double Percentile(std::vector<double> values, double percentile) {
	if (values.empty()) {
		return 0.0;
	}
	std::sort(values.begin(), values.end());
	const auto index = static_cast<size_t>(percentile * static_cast<double>(values.size() - 1));
	return values[index];
}

double ElapsedUs(const Clock::time_point &start, const Clock::time_point &end) {
	return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

std::string GetEnv(const char *name, const std::string &fallback) {
	const char *value = std::getenv(name);
	return value ? value : fallback;
}

int GetEnvInt(const char *name, int fallback) {
	const char *value = std::getenv(name);
	return value ? std::stoi(value) : fallback;
}

double GetEnvDouble(const char *name, double fallback) {
	const char *value = std::getenv(name);
	return value ? std::stod(value) : fallback;
}

bool EnvFlag(const char *name) {
	const char *value = std::getenv(name);
	return value && (std::string(value) == "1" || std::string(value) == "true" || std::string(value) == "yes");
}

void LogProgress(const std::string &message) {
	std::cerr << "[benchmark_aux_workload_multi_connection] " << message << std::endl;
	std::cerr.flush();
}

bool IsSupportedQuery(int query_num) {
	const auto queries = accelerator_codegen::test_support::CrossAccelAllQueries();
	return std::find(queries.begin(), queries.end(), query_num) != queries.end();
}

bool RunInvalidatingWrite(duckdb::Connection &connection, int query_num, const std::string &writer_sql) {
	if (accelerator_codegen::test_support::DetectCrossAccelBenchmark() == CrossAccelBenchmark::SecEdgar) {
		try {
			accelerator_codegen::test_support::ApplyInvalidatingWriterSql(connection, query_num);
			return true;
		} catch (const std::exception &) {
			return false;
		}
	}
	auto result = connection.Query(writer_sql);
	return result && !result->HasError();
}

uint64_t ReaderStrideForWritePct(int write_pct) {
	if (write_pct <= 0) {
		return 0;
	}
	if (write_pct == 1) {
		return 100;
	}
	if (write_pct == 10) {
		return 10;
	}
	if (write_pct == 50) {
		return 2;
	}
	return static_cast<uint64_t>(100 / write_pct);
}

void MaybeThrottleAggregateReaderQps(const Clock::time_point &phase_start, uint64_t reader_queries_finished,
                                     double target_aggregate_qps) {
	if (target_aggregate_qps <= 0.0) {
		return;
	}
	const double elapsed_sec = ElapsedUs(phase_start, Clock::now()) / 1'000'000.0;
	if (elapsed_sec <= 0.0) {
		return;
	}
	const double allowed = elapsed_sec * target_aggregate_qps;
	if (static_cast<double>(reader_queries_finished) <= allowed) {
		return;
	}
	const double sleep_sec = (static_cast<double>(reader_queries_finished) - allowed) / target_aggregate_qps;
	if (sleep_sec > 0.0) {
		std::this_thread::sleep_for(std::chrono::duration<double>(sleep_sec));
	}
}

void ConfigureConnection(duckdb::Connection &connection) {
	connection.Query("PRAGMA threads=1");
}

bool ConsumeQueryResult(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	if (!result || result->HasError()) {
		return false;
	}
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
		for (duckdb::idx_t col = 0; col < materialized.ColumnCount(); col++) {
			(void)materialized.GetValue(col, row);
		}
	}
	return true;
}

bool RunReaderQuery(duckdb::Connection &connection, int query_num, ReaderKind kind) {
	const auto sql = kind == ReaderKind::Baseline
	                     ? accelerator_codegen::test_support::CrossAccelReferenceSql(query_num)
	                     : accelerator_codegen::test_support::CrossAccelAuxSql(query_num);
	return ConsumeQueryResult(connection, sql);
}

bool QueryUsedAuxPath(int query_num) {
	if (accelerator_codegen::test_support::DetectCrossAccelBenchmark() == CrossAccelBenchmark::SecEdgar) {
		switch (query_num) {
		case 1:
			return accelerator_codegen::GetSecEdgarQ1AuxDiagnostics().used_aux_path;
		case 2:
			return accelerator_codegen::GetSecEdgarQ2AuxDiagnostics().used_aux_path;
		case 3:
			return accelerator_codegen::GetSecEdgarQ3AuxDiagnostics().used_aux_path;
		case 4:
			return accelerator_codegen::GetSecEdgarQ4AuxDiagnostics().used_aux_path;
		case 6:
			return accelerator_codegen::GetSecEdgarQ6AuxDiagnostics().used_aux_path;
		case 24:
			return accelerator_codegen::GetSecEdgarQ24AuxDiagnostics().used_aux_path;
		default:
			return false;
		}
	}
	switch (query_num) {
	case 1:
		return accelerator_codegen::GetQ1AuxDiagnostics().used_aux_path;
	case 3:
		return accelerator_codegen::GetQ3AuxDiagnostics().used_aux_path;
	case 6:
		return accelerator_codegen::GetQ6AuxDiagnostics().used_aux_path;
	case 10:
		return accelerator_codegen::GetQ10AuxDiagnostics().used_aux_path;
	case 12:
		return accelerator_codegen::GetQ12AuxDiagnostics().used_aux_path;
	case 14:
		return accelerator_codegen::GetQ14AuxDiagnostics().used_aux_path;
	default:
		return false;
	}
}

void ResetAuxDiagnostics(int query_num) {
	if (accelerator_codegen::test_support::DetectCrossAccelBenchmark() == CrossAccelBenchmark::SecEdgar) {
		switch (query_num) {
		case 1:
			accelerator_codegen::ResetSecEdgarQ1AuxDiagnostics();
			break;
		case 2:
			accelerator_codegen::ResetSecEdgarQ2AuxDiagnostics();
			break;
		case 3:
			accelerator_codegen::ResetSecEdgarQ3AuxDiagnostics();
			break;
		case 4:
			accelerator_codegen::ResetSecEdgarQ4AuxDiagnostics();
			break;
		case 6:
			accelerator_codegen::ResetSecEdgarQ6AuxDiagnostics();
			break;
		case 24:
			accelerator_codegen::ResetSecEdgarQ24AuxDiagnostics();
			break;
		default:
			break;
		}
		return;
	}
	switch (query_num) {
	case 1:
		accelerator_codegen::ResetQ1AuxDiagnostics();
		break;
	case 3:
		accelerator_codegen::ResetQ3AuxDiagnostics();
		break;
	case 6:
		accelerator_codegen::ResetQ6AuxDiagnostics();
		break;
	case 10:
		accelerator_codegen::ResetQ10AuxDiagnostics();
		break;
	case 12:
		accelerator_codegen::ResetQ12AuxDiagnostics();
		break;
	case 14:
		accelerator_codegen::ResetQ14AuxDiagnostics();
		break;
	default:
		break;
	}
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

void RunConcurrentPhase(duckdb::DuckDB &database, int query_num, ReaderKind reader_kind,
                              const std::string &phase_label, int readers, int duration_sec, int write_pct,
                              double write_interval_sec, double aggregate_reader_qps, int writer_bulk_rows,
                              int warmup_queries, PhaseStats &stats) {
	std::atomic<uint64_t> reader_queries_finished {0};
	std::atomic<uint64_t> writer_commits {0};
	std::atomic<uint64_t> writer_total_us {0};
	const bool use_time_based_writes = write_interval_sec > 0.0;
	const uint64_t reader_stride = use_time_based_writes ? 0 : ReaderStrideForWritePct(write_pct);
	const int active_writers = use_time_based_writes || write_pct > 0 ? 1 : 0;
	const auto writer_sql =
	    accelerator_codegen::test_support::InvalidatingWriterSqlForQuery(query_num, writer_bulk_rows);

	LogProgress("starting " + phase_label + " phase for " + std::to_string(duration_sec) + "s write_pct=" +
	            std::to_string(write_pct) + " write_interval_sec=" + std::to_string(write_interval_sec) +
	            " readers=" + std::to_string(readers));

	StartBarrier start_barrier(readers + active_writers);
	const auto phase_deadline = Clock::now() + std::chrono::seconds(duration_sec);
	std::mutex phase_start_mutex;
	Clock::time_point phase_start = Clock::now();
	bool phase_start_set = false;

	struct ConnectionHandle {
		std::mutex mutex;
		duckdb::Connection *connection = nullptr;
	};
	std::vector<std::unique_ptr<ConnectionHandle>> reader_handles;
	reader_handles.reserve(static_cast<size_t>(readers));
	for (int reader_id = 0; reader_id < readers; reader_id++) {
		reader_handles.push_back(std::make_unique<ConnectionHandle>());
	}
	std::unique_ptr<ConnectionHandle> writer_handle;
	if (active_writers > 0) {
		writer_handle = std::make_unique<ConnectionHandle>();
	}

	std::vector<std::thread> threads;
	for (int reader_id = 0; reader_id < readers; reader_id++) {
		auto *handle = reader_handles[static_cast<size_t>(reader_id)].get();
		threads.emplace_back([&, reader_id, handle]() {
			duckdb::Connection connection(database);
			{
				std::lock_guard<std::mutex> lock(handle->mutex);
				handle->connection = &connection;
			}
			if (reader_kind == ReaderKind::Accel) {
				accelerator_codegen::RegisterAcceleratorFunctions(connection);
			}
			ConfigureConnection(connection);
			if (reader_kind == ReaderKind::Accel && warmup_queries > 0) {
				for (int i = 0; i < warmup_queries; i++) {
					(void)RunReaderQuery(connection, query_num, reader_kind);
				}
			}
			start_barrier.ArriveAndWait();
			{
				std::lock_guard<std::mutex> lock(phase_start_mutex);
				if (!phase_start_set) {
					phase_start = Clock::now();
					phase_start_set = true;
				}
			}
			while (Clock::now() < phase_deadline) {
				const auto start = Clock::now();
				if (reader_kind == ReaderKind::Accel) {
					ResetAuxDiagnostics(query_num);
				}
				const bool ok = RunReaderQuery(connection, query_num, reader_kind);
				reader_queries_finished.fetch_add(1);
				const auto us = ElapsedUs(start, Clock::now());
				if (start < phase_deadline) {
					std::lock_guard<std::mutex> lock(stats.latencies_mutex);
					stats.latencies.push_back(us);
				}
				if (ok) {
					stats.successes.fetch_add(1);
					if (reader_kind == ReaderKind::Accel) {
						stats.aux_reads.fetch_add(1);
						if (QueryUsedAuxPath(query_num)) {
							stats.aux_hits.fetch_add(1);
						}
					}
				} else {
					stats.failures.fetch_add(1);
				}
				MaybeThrottleAggregateReaderQps(phase_start, reader_queries_finished.load(), aggregate_reader_qps);
			}
			{
				std::lock_guard<std::mutex> lock(handle->mutex);
				handle->connection = nullptr;
			}
		});
	}

	std::thread writer_thread;
	if (active_writers > 0) {
		writer_thread = std::thread([&]() {
			duckdb::Connection connection(database);
			{
				std::lock_guard<std::mutex> lock(writer_handle->mutex);
				writer_handle->connection = &connection;
			}
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			ConfigureConnection(connection);
			start_barrier.ArriveAndWait();
			if (use_time_based_writes) {
				auto next_write_at = Clock::now();
				while (Clock::now() < phase_deadline) {
					if (Clock::now() < next_write_at) {
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
						continue;
					}
					const auto write_start = Clock::now();
					const bool wrote = RunInvalidatingWrite(connection, query_num, writer_sql);
					writer_total_us.fetch_add(static_cast<uint64_t>(ElapsedUs(write_start, Clock::now())));
					if (wrote) {
						writer_commits.fetch_add(1);
					}
					next_write_at = write_start + std::chrono::duration_cast<Clock::duration>(
					                                    std::chrono::duration<double>(write_interval_sec));
				}
			} else {
			uint64_t writes_done = 0;
			while (Clock::now() < phase_deadline) {
				const uint64_t finished = reader_queries_finished.load();
				const uint64_t target_writes = reader_stride > 0 ? finished / reader_stride : 0;
				if (writes_done >= target_writes) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}
				const auto write_start = Clock::now();
				const bool wrote = RunInvalidatingWrite(connection, query_num, writer_sql);
				writer_total_us.fetch_add(static_cast<uint64_t>(ElapsedUs(write_start, Clock::now())));
				if (wrote) {
					writer_commits.fetch_add(1);
				}
				writes_done++;
			}
			}
			{
				std::lock_guard<std::mutex> lock(writer_handle->mutex);
				writer_handle->connection = nullptr;
			}
		});
	}

	start_barrier.ArriveAndWait();
	for (auto &thread : threads) {
		thread.join();
	}
	if (writer_thread.joinable()) {
		writer_thread.join();
	}

	stats.writer_commits = writer_commits.load();
	stats.writer_total_us = static_cast<double>(writer_total_us.load());
	LogProgress(phase_label + " complete reader_queries=" + std::to_string(stats.successes.load() + stats.failures.load()) +
	            " writer_commits=" + std::to_string(stats.writer_commits));
}

double SumLatencies(PhaseStats &stats) {
	double total = 0.0;
	std::lock_guard<std::mutex> lock(stats.latencies_mutex);
	for (double sample : stats.latencies) {
		total += sample;
	}
	return total;
}

} // namespace

int main(int argc, char **argv) {
	try {
		const int query_num = argc > 1 ? std::stoi(argv[1])
		                               : GetEnvInt("ACCEL_WORKLOAD_QUERY", GetEnvInt("ACCEL_WORKLOAD_MC_QUERY", 0));
		if (!IsSupportedQuery(query_num)) {
			const auto supported = accelerator_codegen::test_support::CrossAccelAllQueries();
			std::string supported_list;
			for (size_t i = 0; i < supported.size(); i++) {
				if (i > 0) {
					supported_list += ',';
				}
				supported_list += std::to_string(supported[i]);
			}
			throw std::runtime_error("unsupported query; use one of " + supported_list);
		}

		const char *db_path =
		    argc > 2 ? argv[2] : accelerator_codegen::test_support::CrossAccelDatabasePath().c_str();
		const int readers = GetEnvInt("ACCEL_WORKLOAD_READERS", GetEnvInt("ACCEL_WORKLOAD_MC_READERS", 4));
		const int duration_sec = GetEnvInt("ACCEL_WORKLOAD_DURATION_SEC", GetEnvInt("ACCEL_WORKLOAD_MC_DURATION_SEC", 25));
		const int write_pct = GetEnvInt("ACCEL_WORKLOAD_WRITE_PCT", GetEnvInt("ACCEL_WORKLOAD_MC_WRITE_PCT", 0));
		const double write_interval_sec =
		    GetEnvDouble("ACCEL_WORKLOAD_WRITE_INTERVAL_SEC", GetEnvDouble("ACCEL_WORKLOAD_MC_WRITE_INTERVAL_SEC", 0.0));
		const int warmup_queries = GetEnvInt("ACCEL_WORKLOAD_WARMUP", GetEnvInt("ACCEL_BENCH_WARMUP", 5));
		const int writer_bulk_rows = GetEnvInt("ACCEL_WORKLOAD_WRITER_BULK_ROWS", 1);
		const double aggregate_reader_qps = GetEnvDouble("ACCEL_WORKLOAD_AGGREGATE_READER_QPS",
		                                                 GetEnvDouble("ACCEL_WORKLOAD_MC_AGGREGATE_READER_QPS", 0.0));
		const bool skip_build = EnvFlag("ACCEL_WORKLOAD_SKIP_BUILD");
		const bool skip_correctness = EnvFlag("ACCEL_WORKLOAD_SKIP_CORRECTNESS");

		const bool use_time_based_writes = write_interval_sec > 0.0;
		if (!use_time_based_writes && write_pct > 0 && write_pct != 1 && write_pct != 10 && write_pct != 50 &&
		    100 % write_pct != 0) {
			throw std::runtime_error("ACCEL_WORKLOAD_WRITE_PCT must be 0, 1, 10, 50, or another divisor of 100");
		}
		if (use_time_based_writes && write_pct > 0) {
			throw std::runtime_error("set only one of ACCEL_WORKLOAD_WRITE_PCT or ACCEL_WORKLOAD_WRITE_INTERVAL_SEC");
		}

		LogProgress("query=Q" + std::to_string(query_num) + " db=" + db_path + " readers=" + std::to_string(readers) +
		            " duration_sec=" + std::to_string(duration_sec) + " write_pct=" + std::to_string(write_pct) +
		            " write_interval_sec=" + std::to_string(write_interval_sec) +
		            " aggregate_reader_qps=" + std::to_string(aggregate_reader_qps));

		duckdb::DuckDB database(db_path);
		{
			duckdb::Connection setup(database);
			accelerator_codegen::RegisterAcceleratorFunctions(setup);
			ConfigureConnection(setup);
			if (!skip_build) {
				LogProgress("building aux state for Q" + std::to_string(query_num));
				accelerator_codegen::test_support::BuildCrossAccelAux(setup, query_num);
			}
			if (!skip_correctness) {
				accelerator_codegen::test_support::RequireCrossAccelQueryCorrectness(
				    setup, query_num, "Q" + std::to_string(query_num));
				LogProgress("correctness check passed");
			}
		}

		PhaseStats baseline;
		PhaseStats accel;
		RunConcurrentPhase(database, query_num, ReaderKind::Baseline, "baseline", readers, duration_sec, write_pct,
		                   write_interval_sec, aggregate_reader_qps, writer_bulk_rows, 0, baseline);
		RunConcurrentPhase(database, query_num, ReaderKind::Accel, "accel", readers, duration_sec, write_pct,
		                   write_interval_sec, aggregate_reader_qps, writer_bulk_rows, warmup_queries, accel);

		std::vector<double> baseline_latencies;
		std::vector<double> accel_latencies;
		{
			std::lock_guard<std::mutex> lock(baseline.latencies_mutex);
			baseline_latencies = baseline.latencies;
		}
		{
			std::lock_guard<std::mutex> lock(accel.latencies_mutex);
			accel_latencies = accel.latencies;
		}

		const uint64_t reader_queries = accel.successes.load() + accel.failures.load();
		const double effective_write_pct =
		    reader_queries > 0 ? 100.0 * static_cast<double>(accel.writer_commits) / static_cast<double>(reader_queries)
		                       : 0.0;
		const double baseline_read_sum = SumLatencies(baseline);
		const double accel_read_sum = SumLatencies(accel);
		const double baseline_workload_us = baseline_read_sum + baseline.writer_total_us;
		const double accel_workload_us = accel_read_sum + accel.writer_total_us;
		const double workload_speedup = accel_workload_us > 0.0 ? baseline_workload_us / accel_workload_us : 0.0;
		const double baseline_p50 = Percentile(baseline_latencies, 0.50);
		const double accel_p50 = Percentile(accel_latencies, 0.50);
		const double baseline_p95 = Percentile(baseline_latencies, 0.95);
		const double accel_p95 = Percentile(accel_latencies, 0.95);
		const double read_speedup_p50 = accel_p50 > 0.0 ? baseline_p50 / accel_p50 : 0.0;
		const double read_speedup_p95 = accel_p95 > 0.0 ? baseline_p95 / accel_p95 : 0.0;
		const uint64_t aux_reads = accel.aux_reads.load();
		const double accel_hit_rate =
		    aux_reads > 0 ? static_cast<double>(accel.aux_hits.load()) / static_cast<double>(aux_reads) : 0.0;

		const double writes_per_sec =
		    duration_sec > 0 ? static_cast<double>(accel.writer_commits) / static_cast<double>(duration_sec) : 0.0;

		std::cout << "query_num,write_frequency_pct,write_interval_sec,effective_write_pct,writes_per_sec,readers,"
		             "duration_sec,aggregate_reader_qps,"
		             "reader_queries,baseline_writer_commits,accel_writer_commits,baseline_read_sum_us,accel_read_sum_us,"
		             "baseline_write_us,accel_write_us,workload_speedup,read_speedup_p50,read_speedup_p95,"
		             "baseline_read_p50_us,accel_read_p50_us,baseline_read_p95_us,accel_read_p95_us,accel_hit_rate\n";
		std::cout << query_num << ',' << write_pct << ',' << write_interval_sec << ',' << effective_write_pct << ','
		          << writes_per_sec << ',' << readers << ',' << duration_sec
		          << ',' << aggregate_reader_qps << ',' << reader_queries << ',' << baseline.writer_commits << ','
		          << accel.writer_commits << ',' << baseline_read_sum << ',' << accel_read_sum << ','
		          << baseline.writer_total_us << ',' << accel.writer_total_us << ',' << workload_speedup << ','
		          << read_speedup_p50 << ',' << read_speedup_p95 << ',' << baseline_p50 << ',' << accel_p50 << ','
		          << baseline_p95 << ',' << accel_p95 << ',' << accel_hit_rate << '\n';
		LogProgress("done workload_speedup=" + std::to_string(workload_speedup) + " read_p50_speedup=" +
		            std::to_string(read_speedup_p50) + " hit_rate=" + std::to_string(accel_hit_rate));
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_aux_workload_multi_connection failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
