#include "accelerator_codegen/register.hpp"
#include "test_support/cross_accel_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kSupportedQueries[] = {1, 3, 6, 10, 12, 14};

enum class ReaderKind { Baseline, Accel };

struct QueryStats {
	std::mutex latencies_mutex;
	std::vector<double> latencies;
	std::atomic<uint64_t> successes {0};
	std::atomic<uint64_t> failures {0};
};

struct ConcurrentPhaseResult {
	std::unordered_map<int, std::unique_ptr<QueryStats>> stats_by_query;
	uint64_t writer_commits = 0;
};

double ElapsedMs(const Clock::time_point &start, const Clock::time_point &end) {
	return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

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

bool EnvFlag(const char *name) {
	const char *value = std::getenv(name);
	return value && (std::string(value) == "1" || std::string(value) == "true" || std::string(value) == "yes");
}

void LogProgress(const std::string &message) {
	std::cerr << "[benchmark_aux_cross_accelerator] " << message << std::endl;
	std::cerr.flush();
}

bool IsSupportedQuery(int query_num) {
	for (int supported : kSupportedQueries) {
		if (supported == query_num) {
			return true;
		}
	}
	return false;
}

std::vector<int> ParseQueryList(const std::string &value) {
	std::vector<int> queries;
	std::stringstream stream(value);
	std::string token;
	while (std::getline(stream, token, ',')) {
		if (token.empty()) {
			continue;
		}
		const int query_num = std::stoi(token);
		if (!IsSupportedQuery(query_num)) {
			throw std::runtime_error("unsupported query in ACCEL_CROSS_ACCEL_QUERIES: " + token);
		}
		queries.push_back(query_num);
	}
	if (queries.empty()) {
		throw std::runtime_error("ACCEL_CROSS_ACCEL_QUERIES must list at least one query");
	}
	return queries;
}

std::string QueryMixLabel(const std::vector<int> &queries) {
	std::ostringstream out;
	for (size_t i = 0; i < queries.size(); i++) {
		if (i != 0) {
			out << ',';
		}
		out << queries[i];
	}
	return out.str();
}

void RunQuery(duckdb::Connection &connection, const std::string &sql, const std::string &label) {
	auto result = connection.Query(sql);
	if (!result || result->HasError()) {
		const auto message = result ? result->GetError() : "null query result";
		throw std::runtime_error(label + " failed: " + message);
	}
}

void ConfigureConnection(duckdb::Connection &connection, const std::string &mode) {
	if (mode == "isolation") {
		RunQuery(connection, "PRAGMA threads=1", "configure isolation threads");
	}
}

std::string BuildSql(int query_num) {
	switch (query_num) {
	case 1:
		return "SELECT * FROM accel_q1_aux_build()";
	case 3:
		return "SELECT * FROM accel_q3_aux_build()";
	case 10:
		return "SELECT * FROM accel_q10_aux_build()";
	case 12:
		return "SELECT * FROM accel_q12_aux_build()";
	case 14:
		return "SELECT * FROM accel_q14_aux_build()";
	case 6:
		return "SELECT * FROM accel_q6_aux_build()";
	default:
		throw std::runtime_error("unsupported query number");
	}
}

void BuildAllAuxStates(duckdb::Connection &connection, const std::vector<int> &queries) {
	for (int query_num : queries) {
		const auto sql = BuildSql(query_num);
		LogProgress("building aux state for Q" + std::to_string(query_num) + " ...");
		const auto start = Clock::now();
		RunQuery(connection, sql, "Q" + std::to_string(query_num) + " aux build");
		LogProgress("finished Q" + std::to_string(query_num) + " aux build in " +
		            std::to_string(ElapsedMs(start, Clock::now())) + " ms");
	}
}

std::string BaselineSql(int query_num) {
	switch (query_num) {
	case 1:
		return "SELECT l_returnflag, l_linestatus, "
		       "sum(l_quantity) AS sum_qty, sum(l_extendedprice) AS sum_base_price, "
		       "sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price, "
		       "sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge, "
		       "avg(l_quantity) AS avg_qty, avg(l_extendedprice) AS avg_price, avg(l_discount) AS avg_disc, "
		       "count(*) AS count_order "
		       "FROM lineitem "
		       "WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY "
		       "GROUP BY l_returnflag, l_linestatus "
		       "ORDER BY l_returnflag, l_linestatus";
	case 3:
		return "SELECT l_orderkey, revenue, o_orderdate, o_shippriority FROM ("
		       "SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, o_orderdate, o_shippriority "
		       "FROM customer, orders, lineitem "
		       "WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey AND l_orderkey = o_orderkey "
		       "AND o_orderdate < DATE '1995-03-15' AND l_shipdate > DATE '1995-03-15' "
		       "GROUP BY l_orderkey, o_orderdate, o_shippriority "
		       "ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority LIMIT 10)";
	case 10:
		return "SELECT c_custkey, c_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue, c_acctbal, n_name, "
		       "c_address, c_phone, c_comment "
		       "FROM customer, orders, lineitem, nation "
		       "WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey "
		       "AND o_orderdate >= DATE '1993-06-01' AND o_orderdate < DATE '1993-06-01' + INTERVAL '3' month "
		       "AND l_returnflag = 'R' AND c_nationkey = n_nationkey "
		       "GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, c_comment "
		       "ORDER BY revenue DESC";
	case 12:
		return "SELECT l_shipmode, "
		       "SUM(CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH' THEN 1 ELSE 0 END) "
		       "AS high_line_count, "
		       "SUM(CASE WHEN o_orderpriority <> '1-URGENT' AND o_orderpriority <> '2-HIGH' THEN 1 ELSE 0 END) "
		       "AS low_line_count "
		       "FROM orders, lineitem "
		       "WHERE o_orderkey = l_orderkey AND l_shipmode IN ('MAIL', 'SHIP') "
		       "AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate "
		       "AND l_receiptdate >= DATE '1995-01-01' "
		       "AND l_receiptdate < DATE '1995-01-01' + INTERVAL '1' year "
		       "GROUP BY l_shipmode ORDER BY l_shipmode";
	case 14:
		return "SELECT 100.00 * SUM(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * (1 - l_discount) ELSE 0 END) / "
		       "SUM(l_extendedprice * (1 - l_discount)) AS promo_revenue "
		       "FROM lineitem, part "
		       "WHERE l_partkey = p_partkey "
		       "AND l_shipdate >= DATE '1995-06-01' AND l_shipdate < DATE '1995-06-01' + INTERVAL '1' month";
	case 6:
		return "SELECT SUM(l_extendedprice * l_discount) AS revenue "
		       "FROM lineitem "
		       "WHERE l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01' "
		       "AND l_discount BETWEEN 0.02 AND 0.04 AND l_quantity < 25";
	default:
		throw std::runtime_error("unsupported query number for baseline");
	}
}

std::string AuxSql(int query_num) {
	switch (query_num) {
	case 1:
		return "SELECT * FROM accel_tpch_q1_aux(90)";
	case 3:
		return "SELECT * FROM accel_tpch_q3_aux('BUILDING', DATE '1995-03-15')";
	case 10:
		return "SELECT * FROM accel_tpch_q10_aux(DATE '1993-06-01')";
	case 12:
		return "SELECT * FROM accel_tpch_q12_aux('MAIL', 'SHIP', DATE '1995-01-01')";
	case 14:
		return "SELECT * FROM accel_tpch_q14_aux(DATE '1995-06-01')";
	case 6:
		return "SELECT revenue FROM accel_tpch_q6_aux(DATE '1995-01-01', 0.03, 25)";
	default:
		throw std::runtime_error("unsupported query number for aux");
	}
}

bool RunReaderQuery(duckdb::Connection &connection, int query_num, ReaderKind kind) {
	const auto sql = kind == ReaderKind::Baseline ? BaselineSql(query_num) : AuxSql(query_num);
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

bool JoinThreadWithTimeout(std::thread &thread, std::chrono::milliseconds timeout) {
	if (!thread.joinable()) {
		return true;
	}
	std::promise<void> joined;
	auto joined_future = joined.get_future();
	std::thread waiter([&]() {
		thread.join();
		joined.set_value();
	});
	if (joined_future.wait_for(timeout) == std::future_status::ready) {
		waiter.join();
		return true;
	}
	thread.detach();
	waiter.detach();
	return false;
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

struct WriterTarget {
	bool round_robin = true;
	int single_query = 0;
};

WriterTarget ParseWriterTarget(const std::string &value) {
	WriterTarget target;
	if (value.empty() || value == "round_robin" || value == "round-robin") {
		return target;
	}
	std::string normalized = value;
	if (!normalized.empty() && (normalized[0] == 'Q' || normalized[0] == 'q')) {
		normalized = normalized.substr(1);
	}
	const int query_num = std::stoi(normalized);
	if (!IsSupportedQuery(query_num)) {
		throw std::runtime_error("unsupported ACCEL_CROSS_ACCEL_WRITER_TARGET: " + value);
	}
	target.round_robin = false;
	target.single_query = query_num;
	return target;
}

std::string WriterTargetLabel(const WriterTarget &target) {
	if (target.round_robin) {
		return "round_robin";
	}
	return "Q" + std::to_string(target.single_query);
}

std::string WriterSqlForTick(const std::vector<int> &query_mix, size_t tick, const char *custom_writer_sql,
                               const WriterTarget &writer_target, int writer_bulk_rows) {
	if (custom_writer_sql != nullptr && *custom_writer_sql) {
		return custom_writer_sql;
	}
	const int query_num =
	    writer_target.round_robin ? query_mix[tick % query_mix.size()] : writer_target.single_query;
	return accelerator_codegen::test_support::InvalidatingWriterSqlForQuery(query_num, writer_bulk_rows);
}

ConcurrentPhaseResult RunConcurrentPhase(duckdb::DuckDB &database, const std::vector<int> &query_mix, ReaderKind reader_kind,
                                         const std::string &phase_label, const std::string &mode,
                                         const std::string &writer_mode, const char *custom_writer_sql, int readers,
                                         int duration_sec, bool enable_writer, int join_grace_sec, int writer_sleep_ms,
                                         int writer_prob_pct, int num_writers, const WriterTarget &writer_target,
                                         int writer_bulk_rows) {
	ConcurrentPhaseResult phase_result;
	for (int query_num : query_mix) {
		phase_result.stats_by_query.emplace(query_num, std::make_unique<QueryStats>());
	}

	std::atomic<uint64_t> writer_commits {0};
	std::atomic<uint64_t> writer_attempts {0};
	std::atomic<uint64_t> reader_queries_started {0};
	std::atomic<uint64_t> reader_queries_finished {0};

	LogProgress("starting " + phase_label + " concurrent phase for " + std::to_string(duration_sec) + " seconds");
	const int active_writers = enable_writer ? std::max(1, num_writers) : 0;
	const int barrier_participants = readers + active_writers;
	StartBarrier start_barrier(barrier_participants);
	const auto phase_deadline = Clock::now() + std::chrono::seconds(duration_sec);

	struct ReaderConnectionHandle {
		std::mutex mutex;
		duckdb::Connection *connection = nullptr;
		int query_num = 0;
	};
	std::vector<std::unique_ptr<ReaderConnectionHandle>> reader_handles;
	reader_handles.reserve(static_cast<size_t>(readers));
	for (int reader = 0; reader < readers; reader++) {
		reader_handles.push_back(std::make_unique<ReaderConnectionHandle>());
		reader_handles.back()->query_num = query_mix[static_cast<size_t>(reader) % query_mix.size()];
	}

	struct WriterConnectionHandle {
		std::mutex mutex;
		duckdb::Connection *connection = nullptr;
	};
	std::vector<std::unique_ptr<WriterConnectionHandle>> writer_handles;
	if (enable_writer) {
		writer_handles.reserve(static_cast<size_t>(active_writers));
		for (int writer = 0; writer < active_writers; writer++) {
			writer_handles.push_back(std::make_unique<WriterConnectionHandle>());
		}
	}

	std::vector<std::thread> threads;
	for (int reader = 0; reader < readers; reader++) {
		const int query_num = reader_handles[static_cast<size_t>(reader)]->query_num;
		auto *reader_handle = reader_handles[static_cast<size_t>(reader)].get();
		threads.emplace_back([&, query_num, reader_handle]() {
			auto &handle = *reader_handle;
			duckdb::Connection connection(database);
			{
				std::lock_guard<std::mutex> lock(handle.mutex);
				handle.connection = &connection;
			}
			if (reader_kind == ReaderKind::Accel) {
				accelerator_codegen::RegisterAcceleratorFunctions(connection);
			}
			ConfigureConnection(connection, mode);
			start_barrier.ArriveAndWait();
			auto &stats = *phase_result.stats_by_query.at(query_num);
			while (Clock::now() < phase_deadline) {
				reader_queries_started.fetch_add(1);
				const auto start = Clock::now();
				if (RunReaderQuery(connection, query_num, reader_kind)) {
					stats.successes.fetch_add(1);
				} else {
					stats.failures.fetch_add(1);
				}
				reader_queries_finished.fetch_add(1);
				const auto us = ElapsedUs(start, Clock::now());
				if (start < phase_deadline) {
					std::lock_guard<std::mutex> lock(stats.latencies_mutex);
					stats.latencies.push_back(us);
				}
			}
			{
				std::lock_guard<std::mutex> lock(handle.mutex);
				handle.connection = nullptr;
			}
		});
	}

	std::vector<std::thread> writer_threads;
	if (enable_writer) {
		for (int writer_id = 0; writer_id < active_writers; writer_id++) {
			auto *writer_handle = writer_handles[static_cast<size_t>(writer_id)].get();
			writer_threads.emplace_back([&, writer_id, writer_handle]() {
				duckdb::Connection connection(database);
				{
					std::lock_guard<std::mutex> lock(writer_handle->mutex);
					writer_handle->connection = &connection;
				}
				accelerator_codegen::RegisterAcceleratorFunctions(connection);
				ConfigureConnection(connection, writer_mode);
				start_barrier.ArriveAndWait();
				size_t tick = static_cast<size_t>(writer_id);
				std::mt19937 rng(static_cast<uint32_t>(
				    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count() ^
				    static_cast<uint32_t>(writer_id * 2654435761u)));
				std::uniform_int_distribution<int> commit_roll(1, 100);
				while (Clock::now() < phase_deadline) {
					writer_attempts.fetch_add(1);
					const bool should_commit = writer_prob_pct >= 100 || commit_roll(rng) <= writer_prob_pct;
					if (should_commit) {
						const auto sql =
						    WriterSqlForTick(query_mix, tick, custom_writer_sql, writer_target, writer_bulk_rows);
						if (sql.empty()) {
							continue;
						}
						auto result = connection.Query(sql);
						if (result && !result->HasError()) {
							writer_commits.fetch_add(1);
						}
					}
					tick += static_cast<size_t>(active_writers);
					if (writer_sleep_ms > 0) {
						std::this_thread::sleep_for(std::chrono::milliseconds(writer_sleep_ms));
					}
				}
				{
					std::lock_guard<std::mutex> lock(writer_handle->mutex);
					writer_handle->connection = nullptr;
				}
			});
		}
	}

	start_barrier.ArriveAndWait();
	if (enable_writer) {
		LogProgress(phase_label + ": reader and writer connections ready; concurrent phase running");
	} else {
		LogProgress(phase_label + ": reader connections ready; writer disabled");
	}

	std::atomic<bool> phase_done {false};
	std::thread progress_ticker([&]() {
		int tick = 0;
		while (!phase_done.load()) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			if (phase_done.load()) {
				break;
			}
			++tick;
			LogProgress(phase_label + " concurrent phase still running (" + std::to_string(tick) + "s elapsed)" +
			            " reader_started=" + std::to_string(reader_queries_started.load()) +
			            " reader_finished=" + std::to_string(reader_queries_finished.load()) +
			            " writer_attempts=" + std::to_string(writer_attempts.load()) +
			            " writer_commits=" + std::to_string(writer_commits.load()));
		}
	});

	std::this_thread::sleep_until(phase_deadline);
	LogProgress(phase_label + " concurrent duration elapsed; interrupting in-flight reader queries");
	phase_done.store(true);
	for (auto &handle : reader_handles) {
		std::lock_guard<std::mutex> lock(handle->mutex);
		if (handle->connection != nullptr) {
			handle->connection->Interrupt();
		}
	}
	for (auto &handle : writer_handles) {
		std::lock_guard<std::mutex> lock(handle->mutex);
		if (handle->connection != nullptr) {
			handle->connection->Interrupt();
		}
	}

	const auto join_grace = std::chrono::seconds(join_grace_sec);
	int hung_readers = 0;
	for (size_t reader = 0; reader < threads.size(); reader++) {
		if (!JoinThreadWithTimeout(threads[reader], join_grace)) {
			hung_readers++;
			LogProgress("warning: " + phase_label + " reader " + std::to_string(reader) + " (Q" +
			            std::to_string(reader_handles[reader]->query_num) +
			            ") did not exit after interrupt; detached");
		}
	}
	if (hung_readers > 0) {
		LogProgress("warning: " + phase_label + " " + std::to_string(hung_readers) +
		            " reader thread(s) detached after join grace (" + std::to_string(join_grace_sec) + "s)");
	}

	if (enable_writer) {
		int hung_writers = 0;
		for (size_t writer = 0; writer < writer_threads.size(); writer++) {
			if (!JoinThreadWithTimeout(writer_threads[writer], join_grace)) {
				hung_writers++;
				LogProgress("warning: " + phase_label + " writer " + std::to_string(writer) +
				            " did not exit after interrupt; detached");
			}
		}
		if (hung_writers > 0) {
			LogProgress("warning: " + phase_label + " " + std::to_string(hung_writers) +
			            " writer thread(s) detached after join grace (" + std::to_string(join_grace_sec) + "s)");
		}
	}
	progress_ticker.join();
	LogProgress(phase_label + " concurrent phase complete");

	phase_result.writer_commits = writer_commits.load();
	return phase_result;
}

} // namespace

int main(int argc, char **argv) {
	const char *db_path = argc > 1 ? argv[1] : "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
	const std::string mode = argc > 2 ? argv[2] : GetEnv("ACCEL_CROSS_ACCEL_MODE", GetEnv("ACCEL_MULTI_CONN_MODE", "isolation"));
	const int readers = argc > 3 ? std::stoi(argv[3]) : GetEnvInt("ACCEL_CROSS_ACCEL_READERS", 5);
	const int duration_sec = GetEnvInt("ACCEL_CROSS_ACCEL_DURATION_SEC", GetEnvInt("ACCEL_MULTI_CONN_DURATION_SEC", 30));
	const auto query_mix = ParseQueryList(GetEnv("ACCEL_CROSS_ACCEL_QUERIES", "3,14"));
	const std::string build_mode = GetEnv("ACCEL_CROSS_ACCEL_BUILD_MODE", "warehouse");
	const std::string writer_mode =
	    GetEnv("ACCEL_CROSS_ACCEL_WRITER_MODE", mode);
	const char *custom_writer_sql = std::getenv("ACCEL_CROSS_ACCEL_WRITER_SQL");
	const bool skip_build = EnvFlag("ACCEL_CROSS_ACCEL_SKIP_BUILD");
	const bool enable_writer = !EnvFlag("ACCEL_CROSS_ACCEL_DISABLE_WRITER");
	const bool skip_baseline = EnvFlag("ACCEL_CROSS_ACCEL_SKIP_BASELINE");
	const bool skip_correctness = EnvFlag("ACCEL_CROSS_ACCEL_SKIP_CORRECTNESS");
	const int join_grace_sec = GetEnvInt("ACCEL_CROSS_ACCEL_JOIN_GRACE_SEC", 15);
	const int writer_sleep_ms = GetEnvInt("ACCEL_CROSS_ACCEL_WRITER_SLEEP_MS", 200);
	const int writer_prob_pct = GetEnvInt("ACCEL_CROSS_ACCEL_WRITER_PROB_PCT", 100);
	const int num_writers = GetEnvInt("ACCEL_CROSS_ACCEL_NUM_WRITERS", 1);
	const int writer_bulk_rows = GetEnvInt("ACCEL_CROSS_ACCEL_WRITER_BULK_ROWS", 1);
	const WriterTarget writer_target = ParseWriterTarget(GetEnv("ACCEL_CROSS_ACCEL_WRITER_TARGET", "round_robin"));

	try {
		if (readers <= 0) {
			throw std::runtime_error("reader count must be positive");
		}

		if (enable_writer && num_writers <= 0) {
			throw std::runtime_error("ACCEL_CROSS_ACCEL_NUM_WRITERS must be positive when writer is enabled");
		}
		if (writer_prob_pct < 0 || writer_prob_pct > 100) {
			throw std::runtime_error("ACCEL_CROSS_ACCEL_WRITER_PROB_PCT must be in [0, 100]");
		}

		LogProgress("starting db=" + std::string(db_path) + " mode=" + mode + " build_mode=" + build_mode +
		            " writer_mode=" + writer_mode + " readers=" +
		            std::to_string(readers) + " writers=" + std::to_string(enable_writer ? num_writers : 0) +
		            " duration_sec=" + std::to_string(duration_sec) + " queries=" + QueryMixLabel(query_mix) +
		            " writer_sleep_ms=" + std::to_string(writer_sleep_ms) +
		            " writer_prob_pct=" + std::to_string(writer_prob_pct) +
		            " writer_bulk_rows=" + std::to_string(writer_bulk_rows) +
		            " writer_target=" + WriterTargetLabel(writer_target));
		if (enable_writer) {
			if (custom_writer_sql != nullptr && *custom_writer_sql) {
				LogProgress("using custom ACCEL_CROSS_ACCEL_WRITER_SQL");
			} else {
				LogProgress("using per-query invalidating UPDATEs on watched columns");
			}
		}

		duckdb::DuckDB database(db_path);
		{
			duckdb::Connection setup(database);
			accelerator_codegen::RegisterAcceleratorFunctions(setup);
			ConfigureConnection(setup, build_mode);
			if (skip_build) {
				LogProgress("skipping aux builds (ACCEL_CROSS_ACCEL_SKIP_BUILD=1)");
			} else {
				BuildAllAuxStates(setup, query_mix);
			}
		}
		LogProgress("setup connection closed; aux state lives in shared registry");

		if (!skip_correctness) {
			LogProgress("running differential correctness check before concurrent phases ...");
			duckdb::Connection check(database);
			accelerator_codegen::RegisterAcceleratorFunctions(check);
			ConfigureConnection(check, mode);
			std::vector<int> check_order(query_mix.begin(), query_mix.end());
			std::sort(check_order.begin(), check_order.end());
			accelerator_codegen::test_support::RequireCrossAccelCorrectness(check, check_order);
			LogProgress("pre-run differential correctness check passed");
		}

		ConcurrentPhaseResult baseline_result;
		if (!skip_baseline) {
			baseline_result = RunConcurrentPhase(database, query_mix, ReaderKind::Baseline, "baseline", mode,
			                                       writer_mode, custom_writer_sql, readers, duration_sec,
			                                       enable_writer, join_grace_sec, writer_sleep_ms, writer_prob_pct,
			                                       num_writers, writer_target, writer_bulk_rows);
			std::vector<int> baseline_order(query_mix.begin(), query_mix.end());
			std::sort(baseline_order.begin(), baseline_order.end());
			for (int query_num : baseline_order) {
				const auto &stats = *baseline_result.stats_by_query.at(query_num);
				const auto p50 = Percentile(stats.latencies, 0.50);
				LogProgress("concurrent base DuckDB Q" + std::to_string(query_num) + " p50=" + std::to_string(p50) +
				            " us");
			}
		}

		const auto accel_result = RunConcurrentPhase(database, query_mix, ReaderKind::Accel, "accel", mode, writer_mode,
		                                           custom_writer_sql, readers, duration_sec, enable_writer,
		                                           join_grace_sec, writer_sleep_ms, writer_prob_pct, num_writers,
		                                           writer_target, writer_bulk_rows);

		if (!skip_correctness) {
			LogProgress("running differential correctness check after accel phase ...");
			duckdb::Connection check(database);
			accelerator_codegen::RegisterAcceleratorFunctions(check);
			ConfigureConnection(check, mode);
			std::vector<int> check_order(query_mix.begin(), query_mix.end());
			std::sort(check_order.begin(), check_order.end());
			accelerator_codegen::test_support::RequireCrossAccelCorrectness(check, check_order);
			LogProgress("differential correctness check passed for all queries in mix");
		}

		const auto elapsed_sec = static_cast<double>(duration_sec);

		uint64_t total_accel_reader_queries = 0;
		for (const auto &entry : accel_result.stats_by_query) {
			total_accel_reader_queries +=
			    entry.second->successes.load() + entry.second->failures.load();
		}
		const double effective_write_pct =
		    total_accel_reader_queries > 0
		        ? 100.0 * static_cast<double>(accel_result.writer_commits) /
		              static_cast<double>(total_accel_reader_queries)
		        : 0.0;

		std::cout << "benchmark_mode,duration_sec,readers,num_writers,writer_target,baseline_writer_commits,"
		             "accel_writer_commits,writer_sleep_ms,writer_prob_pct,effective_write_pct,query_num,"
		             "accel_samples,accel_throughput_qps,accel_p50_us,accel_p95_us,accel_hit_rate,"
		             "duckdb_concurrent_p50_us,speedup_p50\n";
		std::cout.setf(std::ios::unitbuf);

		std::vector<int> output_order(query_mix.begin(), query_mix.end());
		std::sort(output_order.begin(), output_order.end());
		for (int query_num : output_order) {
			const auto &stats = *accel_result.stats_by_query.at(query_num);
			const auto samples = stats.latencies.size();
			const auto total = stats.successes.load() + stats.failures.load();
			const auto throughput = elapsed_sec > 0.0 ? static_cast<double>(total) / elapsed_sec : 0.0;
			const auto hit_rate =
			    total > 0 ? static_cast<double>(stats.successes.load()) / static_cast<double>(total) : 0.0;
			const auto accel_p50 = Percentile(stats.latencies, 0.50);
			double duckdb_p50 = 0.0;
			if (!skip_baseline) {
				const auto &baseline_stats = *baseline_result.stats_by_query.at(query_num);
				duckdb_p50 = Percentile(baseline_stats.latencies, 0.50);
			}
			const auto speedup = accel_p50 > 0.0 && duckdb_p50 > 0.0 ? duckdb_p50 / accel_p50 : 0.0;
			std::cout << mode << ',' << duration_sec << ',' << readers << ',' << (enable_writer ? num_writers : 0)
			          << ',' << WriterTargetLabel(writer_target) << ',' << baseline_result.writer_commits << ','
			          << accel_result.writer_commits << ',' << writer_sleep_ms << ',' << writer_prob_pct << ','
			          << effective_write_pct << ',' << query_num << ',' << samples << ',' << throughput << ','
			          << accel_p50 << ',' << Percentile(stats.latencies, 0.95) << ',' << hit_rate << ',' << duckdb_p50
			          << ',' << speedup << '\n';
		}
		LogProgress("done");
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_aux_cross_accelerator failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
