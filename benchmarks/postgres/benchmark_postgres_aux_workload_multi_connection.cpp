#include "accelerator_codegen/aux_state/postgres/postgres_aux_registration.hpp"
#include "accelerator_codegen/aux_state/postgres/postgres_aux_session.hpp"
#include "accelerator_codegen/aux_state/postgres/postgres_mutation_triggers.hpp"
#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/sec_edgar_q24_aux.hpp"
#include "accelerator_codegen/tpch_q10_aux.hpp"
#include "test_support/cross_accel_helpers.hpp"
#include "test_support/q10_aux_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "libpq-fe.h"

namespace {

using Clock = std::chrono::steady_clock;

enum class BenchmarkSuite { Q10, Q24 };

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
	std::cerr << "[benchmark_postgres_aux_workload] " << message << std::endl;
	std::cerr.flush();
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

BenchmarkSuite DetectSuite(int query_num) {
	if (query_num == 10) {
		return BenchmarkSuite::Q10;
	}
	if (query_num == 24) {
		return BenchmarkSuite::Q24;
	}
	throw std::runtime_error("unsupported query; use 10 (TPC-H Q10) or 24 (SEC EDGAR Q24)");
}

std::string BaselineSql(BenchmarkSuite suite) {
	if (suite == BenchmarkSuite::Q10) {
		return accelerator_codegen::test_support::ReferenceQ10Sql("1993-06-01");
	}
	return accelerator_codegen::test_support::CrossAccelReferenceSql(24);
}

std::string PostgresReferenceSql(BenchmarkSuite suite) {
	if (suite == BenchmarkSuite::Q10) {
		return "SELECT c_custkey, c_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue, c_acctbal, n_name, "
		       "c_address, c_phone, c_comment "
		       "FROM customer, orders, lineitem, nation "
		       "WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND c_nationkey = n_nationkey "
		       "AND o_orderdate >= DATE '1993-06-01' AND o_orderdate < DATE '1993-06-01' + interval '3' month "
		       "AND l_returnflag = 'R' "
		       "GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, c_comment "
		       "ORDER BY revenue DESC, c_custkey";
	}
	return "SELECT n.tag, n.version, COUNT(*) AS cnt, SUM(n.value) AS total "
	       "FROM num n "
	       "LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version AND n.adsh = p.adsh "
	       "WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 AND 20231231 "
	       "AND n.value IS NOT NULL AND p.adsh IS NULL "
	       "GROUP BY n.tag, n.version "
	       "HAVING COUNT(*) > 10 "
	       "ORDER BY cnt DESC "
	       "LIMIT 100";
}

bool ConsumePgQuery(PGconn &conn, const std::string &sql) {
	PGresult *result = PQexec(&conn, sql.c_str());
	if (result == nullptr || PQresultStatus(result) != PGRES_TUPLES_OK) {
		PQclear(result);
		return false;
	}
	const int rows = PQntuples(result);
	const int cols = PQnfields(result);
	for (int row = 0; row < rows; row++) {
		for (int col = 0; col < cols; col++) {
			(void)PQgetvalue(result, row, col);
		}
	}
	PQclear(result);
	return true;
}

PGconn *ConnectPostgresOrThrow(const std::string &conninfo) {
	PGconn *conn = PQconnectdb(conninfo.c_str());
	if (!conn || PQstatus(conn) != CONNECTION_OK) {
		std::string error = conn ? PQerrorMessage(conn) : "null connection";
		if (conn) {
			PQfinish(conn);
		}
		throw std::runtime_error("postgres connect failed: " + error);
	}
	return conn;
}

std::string AccelSql(BenchmarkSuite suite) {
	if (suite == BenchmarkSuite::Q10) {
		return accelerator_codegen::test_support::AcceleratorQ10AuxSql("1993-06-01");
	}
	return "SELECT * FROM accel_sec_edgar_q24_aux()";
}

bool QueryUsedAuxPath(BenchmarkSuite suite) {
	if (suite == BenchmarkSuite::Q10) {
		return accelerator_codegen::GetQ10AuxDiagnostics().used_aux_path;
	}
	return accelerator_codegen::GetSecEdgarQ24AuxDiagnostics().used_aux_path;
}

void ResetAuxDiagnostics(BenchmarkSuite suite) {
	if (suite == BenchmarkSuite::Q10) {
		accelerator_codegen::ResetQ10AuxDiagnostics();
	} else {
		accelerator_codegen::ResetSecEdgarQ24AuxDiagnostics();
	}
}

std::string InvalidatingWriterSql(BenchmarkSuite suite, int write_seq) {
	if (suite == BenchmarkSuite::Q10) {
		return accelerator_codegen::test_support::InvalidatingWriterSqlForQuery(10, 1, write_seq);
	}
	setenv("ACCEL_AUX_BENCHMARK", "sec_edgar", 1);
	return accelerator_codegen::test_support::InvalidatingWriterSqlForQuery(24, 1, write_seq);
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

std::string SanitizeCopyField(const std::string &value) {
	std::string sanitized;
	for (const char ch : value) {
		if (ch == '\t' || ch == '\n' || ch == '\r' || ch == '\\') {
			sanitized += ' ';
		} else {
			sanitized += ch;
		}
	}
	return sanitized;
}

void CopyTableFromDuckdb(duckdb::Connection &duck, PGconn &pg_conn, const std::string &table,
                         const std::string &select_sql) {
	auto count_result = duck.Query("SELECT COUNT(*) FROM (" + select_sql + ") t");
	if (!count_result || count_result->HasError()) {
		throw std::runtime_error("duckdb count failed for " + table);
	}
	auto &count_mat = count_result->Cast<duckdb::MaterializedQueryResult>();
	const auto row_count = count_mat.GetValue(0, 0).GetValue<int64_t>();
	LogProgress("copying " + table + " (" + std::to_string(row_count) + " rows)");

	PGresult *truncate = PQexec(&pg_conn, ("TRUNCATE " + table).c_str());
	if (truncate == nullptr || PQresultStatus(truncate) != PGRES_COMMAND_OK) {
		std::string error = PQerrorMessage(&pg_conn);
		if (truncate) {
			error += PQresultErrorMessage(truncate);
		}
		PQclear(truncate);
		throw std::runtime_error("postgres truncate failed for " + table + ": " + error);
	}
	PQclear(truncate);

	if (row_count == 0) {
		return;
	}

	const std::string copy_sql = "COPY " + table + " FROM STDIN";
	PGresult *copy_start = PQexec(&pg_conn, copy_sql.c_str());
	if (copy_start == nullptr || PQresultStatus(copy_start) != PGRES_COPY_IN) {
		std::string error = PQerrorMessage(&pg_conn);
		if (copy_start) {
			error += PQresultErrorMessage(copy_start);
		}
		PQclear(copy_start);
		throw std::runtime_error("postgres COPY FROM STDIN failed for " + table + ": " + error);
	}
	PQclear(copy_start);

	auto result = duck.Query(select_sql);
	if (!result || result->HasError()) {
		PQputCopyEnd(&pg_conn, "duckdb select failed");
		throw std::runtime_error("duckdb select failed for " + table);
	}
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	std::ostringstream line;
	for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
		line.str("");
		line.clear();
		for (duckdb::idx_t col = 0; col < materialized.ColumnCount(); col++) {
			if (col > 0) {
				line << '\t';
			}
			const auto value = materialized.GetValue(col, row);
			if (value.IsNull()) {
				continue;
			}
			line << SanitizeCopyField(value.ToString());
		}
		const std::string payload = line.str() + "\n";
		if (PQputCopyData(&pg_conn, payload.c_str(), static_cast<int>(payload.size())) != 1) {
			PQputCopyEnd(&pg_conn, PQerrorMessage(&pg_conn));
			throw std::runtime_error("postgres putCopyData failed for " + table);
		}
	}

	if (PQputCopyEnd(&pg_conn, nullptr) != 1) {
		throw std::runtime_error("postgres putCopyEnd failed for " + table + ": " + PQerrorMessage(&pg_conn));
	}
	PGresult *copy_end = PQgetResult(&pg_conn);
	if (copy_end == nullptr || PQresultStatus(copy_end) != PGRES_COMMAND_OK) {
		std::string error = PQerrorMessage(&pg_conn);
		if (copy_end) {
			error += PQresultErrorMessage(copy_end);
		}
		PQclear(copy_end);
		throw std::runtime_error("postgres COPY end failed for " + table + ": " + error);
	}
	PQclear(copy_end);
}

void LoadTable(duckdb::Connection &duck, accelerator_codegen::aux_state::PostgresAuxSession &session,
               const std::string &table, const std::string &select_sql) {
	CopyTableFromDuckdb(duck, session.Connection(), table, select_sql);
}

void CreateQ10Schema(accelerator_codegen::aux_state::PostgresAuxSession &session) {
	session.Execute(
	    "CREATE TABLE IF NOT EXISTS nation(n_nationkey INTEGER NOT NULL, n_name VARCHAR NOT NULL, n_regionkey INTEGER NOT NULL, "
	    "n_comment VARCHAR NOT NULL)");
	session.Execute(
	    "CREATE TABLE IF NOT EXISTS customer(c_custkey BIGINT NOT NULL, c_name VARCHAR NOT NULL, c_address VARCHAR NOT NULL, "
	    "c_nationkey INTEGER NOT NULL, c_phone VARCHAR NOT NULL, c_acctbal DECIMAL(15,2) NOT NULL, "
	    "c_mktsegment VARCHAR NOT NULL, c_comment VARCHAR NOT NULL)");
	session.Execute(
	    "CREATE TABLE IF NOT EXISTS orders(o_orderkey BIGINT NOT NULL, o_custkey BIGINT NOT NULL, o_orderstatus VARCHAR NOT NULL, "
	    "o_totalprice DECIMAL(15,2) NOT NULL, o_orderdate DATE NOT NULL, o_orderpriority VARCHAR NOT NULL, "
	    "o_clerk VARCHAR NOT NULL, o_shippriority INTEGER NOT NULL, o_comment VARCHAR NOT NULL)");
	session.Execute(
	    "CREATE TABLE IF NOT EXISTS lineitem(l_orderkey BIGINT NOT NULL, l_partkey BIGINT NOT NULL, l_suppkey BIGINT NOT NULL, "
	    "l_linenumber BIGINT NOT NULL, l_quantity DECIMAL(15,2) NOT NULL, l_extendedprice DECIMAL(15,2) NOT NULL, "
	    "l_discount DECIMAL(15,2) NOT NULL, l_tax DECIMAL(15,2) NOT NULL, l_returnflag VARCHAR NOT NULL, "
	    "l_linestatus VARCHAR NOT NULL, l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL, "
	    "l_receiptdate DATE NOT NULL, l_shipinstruct VARCHAR NOT NULL, l_shipmode VARCHAR NOT NULL, "
	    "l_comment VARCHAR NOT NULL)");
	session.Commit();
}

void CreateQ24Schema(accelerator_codegen::aux_state::PostgresAuxSession &session) {
	session.Execute(
	    "CREATE TABLE IF NOT EXISTS pre(adsh VARCHAR, report INTEGER, line INTEGER, stmt VARCHAR, inpth INTEGER, "
	    "rfile VARCHAR, tag VARCHAR, version VARCHAR, plabel VARCHAR, negating INTEGER)");
	session.Execute(
	    "CREATE TABLE IF NOT EXISTS num(adsh VARCHAR, tag VARCHAR, version VARCHAR, ddate INTEGER, qtrs INTEGER, "
	    "uom VARCHAR, coreg VARCHAR, value DOUBLE PRECISION, footnote VARCHAR)");
	session.Commit();
}

void LoadBenchmarkData(BenchmarkSuite suite, const std::string &duckdb_path,
                       accelerator_codegen::aux_state::PostgresAuxSession &session) {
	duckdb::DuckDB database(duckdb_path);
	duckdb::Connection duck(database);
	ConfigureConnection(duck);

	if (suite == BenchmarkSuite::Q10) {
		CreateQ10Schema(session);
		LoadTable(duck, session, "nation", "SELECT * FROM nation");
		LoadTable(duck, session, "customer", "SELECT * FROM customer");
		LoadTable(duck, session, "orders", "SELECT * FROM orders");
		LoadTable(duck, session, "lineitem", "SELECT * FROM lineitem");
		return;
	}

	CreateQ24Schema(session);
	const int num_limit = GetEnvInt("ACCEL_POSTGRES_LOAD_NUM_LIMIT", 500000);
	std::string num_sql =
	    "SELECT * FROM num WHERE uom = 'USD' AND ddate BETWEEN 20230101 AND 20231231 AND value IS NOT NULL";
	if (num_limit > 0) {
		num_sql += " LIMIT " + std::to_string(num_limit);
	}
	LoadTable(duck, session, "num", num_sql);
}

void SetupPostgresAccel(BenchmarkSuite suite, accelerator_codegen::aux_state::PostgresAuxSession &session) {
	if (suite == BenchmarkSuite::Q10) {
		accelerator_codegen::test_support::EnableHybridMaintenance();
		accelerator_codegen::RegisterQ10AuxAcceleratorPostgres(session);
		accelerator_codegen::aux_state::PostgresQ10TxnLifecycleBackend backend;
		backend.Install(session);
	} else {
		accelerator_codegen::RegisterSecEdgarQ24AuxAcceleratorPostgres(session);
		accelerator_codegen::aux_state::PostgresQ24TxnLifecycleBackend backend;
		backend.Install(session);
	}
	session.EnsureScanBridge();
	auto &connection = session.ScanConnection();
	accelerator_codegen::RegisterAcceleratorFunctions(connection);
	ConfigureConnection(connection);
	if (suite == BenchmarkSuite::Q10) {
		session.WithMutationContextTransaction(accelerator_codegen::BuildQ10AuxState);
	} else {
		session.WithMutationContextTransaction(accelerator_codegen::BuildSecEdgarQ24AuxState);
	}
}

void RunDuckdbBaselinePhase(duckdb::DuckDB &database, BenchmarkSuite suite, int readers, int duration_sec,
                            double write_interval_sec, double aggregate_reader_qps, PhaseStats &stats) {
	std::atomic<uint64_t> reader_queries_finished {0};
	std::atomic<uint64_t> writer_commits {0};
	std::atomic<uint64_t> writer_total_us {0};
	const bool use_time_based_writes = write_interval_sec > 0.0;
	const int active_writers = use_time_based_writes ? 1 : 0;
	int write_seq = 0;

	LogProgress("starting baseline phase");
	StartBarrier start_barrier(readers + active_writers);
	const auto phase_deadline = Clock::now() + std::chrono::seconds(duration_sec);
	std::mutex phase_start_mutex;
	Clock::time_point phase_start = Clock::now();
	bool phase_start_set = false;

	std::vector<std::thread> threads;
	for (int reader_id = 0; reader_id < readers; reader_id++) {
		threads.emplace_back([&]() {
			duckdb::Connection connection(database);
			ConfigureConnection(connection);
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
				const bool ok = ConsumeQueryResult(connection, BaselineSql(suite));
				reader_queries_finished.fetch_add(1);
				const auto us = ElapsedUs(start, Clock::now());
				if (start < phase_deadline) {
					std::lock_guard<std::mutex> lock(stats.latencies_mutex);
					stats.latencies.push_back(us);
				}
				if (ok) {
					stats.successes.fetch_add(1);
				} else {
					stats.failures.fetch_add(1);
				}
				MaybeThrottleAggregateReaderQps(phase_start, reader_queries_finished.load(), aggregate_reader_qps);
			}
		});
	}

	std::thread writer_thread;
	if (active_writers > 0) {
		writer_thread = std::thread([&]() {
			duckdb::Connection connection(database);
			ConfigureConnection(connection);
			start_barrier.ArriveAndWait();
			auto next_write_at = Clock::now();
			while (Clock::now() < phase_deadline) {
				if (Clock::now() < next_write_at) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}
				write_seq++;
				const auto write_start = Clock::now();
				const auto sql = InvalidatingWriterSql(suite, write_seq);
				auto result = connection.Query(sql);
				const bool wrote = result && !result->HasError();
				writer_total_us.fetch_add(static_cast<uint64_t>(ElapsedUs(write_start, Clock::now())));
				if (wrote) {
					writer_commits.fetch_add(1);
				}
				next_write_at = write_start + std::chrono::duration_cast<Clock::duration>(
				                                    std::chrono::duration<double>(write_interval_sec));
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
}

void RunPostgresBaselinePhase(const std::string &conninfo, BenchmarkSuite suite, int readers, int duration_sec,
                              double write_interval_sec, double aggregate_reader_qps, PhaseStats &stats) {
	std::atomic<uint64_t> reader_queries_finished {0};
	std::atomic<uint64_t> writer_commits {0};
	std::atomic<uint64_t> writer_total_us {0};
	const bool use_time_based_writes = write_interval_sec > 0.0;
	const int active_writers = use_time_based_writes ? 1 : 0;
	int write_seq = 0;
	const std::string reference_sql = PostgresReferenceSql(suite);

	LogProgress("starting postgres reference baseline phase");
	StartBarrier start_barrier(readers + active_writers);
	const auto phase_deadline = Clock::now() + std::chrono::seconds(duration_sec);
	std::mutex phase_start_mutex;
	Clock::time_point phase_start = Clock::now();
	bool phase_start_set = false;

	std::vector<std::thread> threads;
	for (int reader_id = 0; reader_id < readers; reader_id++) {
		threads.emplace_back([&]() {
			PGconn *conn = ConnectPostgresOrThrow(conninfo);
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
				const bool ok = ConsumePgQuery(*conn, reference_sql);
				reader_queries_finished.fetch_add(1);
				const auto us = ElapsedUs(start, Clock::now());
				if (start < phase_deadline) {
					std::lock_guard<std::mutex> lock(stats.latencies_mutex);
					stats.latencies.push_back(us);
				}
				if (ok) {
					stats.successes.fetch_add(1);
				} else {
					stats.failures.fetch_add(1);
				}
				MaybeThrottleAggregateReaderQps(phase_start, reader_queries_finished.load(), aggregate_reader_qps);
			}
			PQfinish(conn);
		});
	}

	std::thread writer_thread;
	if (active_writers > 0) {
		writer_thread = std::thread([&]() {
			PGconn *conn = ConnectPostgresOrThrow(conninfo);
			start_barrier.ArriveAndWait();
			auto next_write_at = Clock::now();
			while (Clock::now() < phase_deadline) {
				if (Clock::now() < next_write_at) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}
				write_seq++;
				const auto write_start = Clock::now();
				const auto sql = InvalidatingWriterSql(suite, write_seq);
				PGresult *result = PQexec(conn, sql.c_str());
				const bool wrote =
				    result != nullptr && (PQresultStatus(result) == PGRES_TUPLES_OK || PQresultStatus(result) == PGRES_COMMAND_OK);
				PQclear(result);
				writer_total_us.fetch_add(static_cast<uint64_t>(ElapsedUs(write_start, Clock::now())));
				if (wrote) {
					writer_commits.fetch_add(1);
				}
				next_write_at = write_start + std::chrono::duration_cast<Clock::duration>(
				                                    std::chrono::duration<double>(write_interval_sec));
			}
			PQfinish(conn);
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
}

void RunPostgresAccelPhase(accelerator_codegen::aux_state::PostgresAuxSession &session, BenchmarkSuite suite,
                           int readers, int duration_sec, double write_interval_sec, double aggregate_reader_qps,
                           int warmup_queries, PhaseStats &stats) {
	std::atomic<uint64_t> reader_queries_finished {0};
	std::atomic<uint64_t> writer_commits {0};
	std::atomic<uint64_t> writer_total_us {0};
	const bool use_time_based_writes = write_interval_sec > 0.0;
	const int active_writers = use_time_based_writes ? 1 : 0;
	int write_seq = 0;
	std::mutex session_mutex;

	LogProgress("starting postgres accel phase");
	StartBarrier start_barrier(readers + active_writers);
	const auto phase_deadline = Clock::now() + std::chrono::seconds(duration_sec);
	std::mutex phase_start_mutex;
	Clock::time_point phase_start = Clock::now();
	bool phase_start_set = false;

	std::vector<std::thread> threads;
	for (int reader_id = 0; reader_id < readers; reader_id++) {
		threads.emplace_back([&]() {
			auto connection = session.NewScanConnection();
			accelerator_codegen::RegisterAcceleratorFunctions(*connection);
			ConfigureConnection(*connection);
			for (int i = 0; i < warmup_queries; i++) {
				(void)ConsumeQueryResult(*connection, AccelSql(suite));
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
				ResetAuxDiagnostics(suite);
				const bool ok = ConsumeQueryResult(*connection, AccelSql(suite));
				reader_queries_finished.fetch_add(1);
				const auto us = ElapsedUs(start, Clock::now());
				if (start < phase_deadline) {
					std::lock_guard<std::mutex> lock(stats.latencies_mutex);
					stats.latencies.push_back(us);
				}
				if (ok) {
					stats.successes.fetch_add(1);
					stats.aux_reads.fetch_add(1);
					if (QueryUsedAuxPath(suite)) {
						stats.aux_hits.fetch_add(1);
					}
				} else {
					stats.failures.fetch_add(1);
				}
				MaybeThrottleAggregateReaderQps(phase_start, reader_queries_finished.load(), aggregate_reader_qps);
			}
		});
	}

	std::thread writer_thread;
	if (active_writers > 0) {
		writer_thread = std::thread([&]() {
			start_barrier.ArriveAndWait();
			auto next_write_at = Clock::now();
			while (Clock::now() < phase_deadline) {
				if (Clock::now() < next_write_at) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}
				write_seq++;
				const auto write_start = Clock::now();
				const auto sql = InvalidatingWriterSql(suite, write_seq);
				bool wrote = false;
				try {
					std::lock_guard<std::mutex> lock(session_mutex);
					session.BeginTransaction();
					session.Execute(sql);
					session.Commit();
					wrote = true;
				} catch (...) {
					try {
						std::lock_guard<std::mutex> lock(session_mutex);
						session.Rollback();
					} catch (...) {
					}
				}
				writer_total_us.fetch_add(static_cast<uint64_t>(ElapsedUs(write_start, Clock::now())));
				if (wrote) {
					writer_commits.fetch_add(1);
				}
				next_write_at = write_start + std::chrono::duration_cast<Clock::duration>(
				                                    std::chrono::duration<double>(write_interval_sec));
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
		const int query_num = argc > 1 ? std::stoi(argv[1]) : GetEnvInt("ACCEL_WORKLOAD_QUERY", 10);
		const BenchmarkSuite suite = DetectSuite(query_num);
		const std::string duckdb_path = argc > 2 ? argv[2] : GetEnv("ACCEL_WORKLOAD_DUCKDB_PATH", "");
		const std::string conninfo = GetEnv("ACCEL_POSTGRES_CONNINFO",
		                                    "dbname=accel_aux_bench host=localhost port=5434 user=postgres");
		const int readers = GetEnvInt("ACCEL_WORKLOAD_READERS", 4);
		const int duration_sec = GetEnvInt("ACCEL_WORKLOAD_DURATION_SEC", 60);
		const double write_interval_sec = GetEnvDouble("ACCEL_WORKLOAD_WRITE_INTERVAL_SEC", 0.0);
		const int warmup_queries = GetEnvInt("ACCEL_WORKLOAD_WARMUP", 5);
		const double aggregate_reader_qps = GetEnvDouble("ACCEL_WORKLOAD_AGGREGATE_READER_QPS", 0.0);
		const bool load_data = EnvFlag("ACCEL_POSTGRES_LOAD_DATA");
		const bool skip_baseline = EnvFlag("ACCEL_POSTGRES_SKIP_BASELINE");
		const bool duckdb_baseline = EnvFlag("ACCEL_POSTGRES_DUCKDB_BASELINE");

		if (duckdb_path.empty()) {
			throw std::runtime_error("duckdb path required (argv[2] or ACCEL_WORKLOAD_DUCKDB_PATH)");
		}

		accelerator_codegen::aux_state::PostgresConnectionConfig config;
		config.conninfo = conninfo;
		accelerator_codegen::aux_state::PostgresAuxSession session(config);

		if (load_data) {
			LogProgress("loading benchmark data into postgres");
			LoadBenchmarkData(suite, duckdb_path, session);
			LogProgress("data load complete");
			return EXIT_SUCCESS;
		}

		SetupPostgresAccel(suite, session);

		PhaseStats baseline;
		if (!skip_baseline) {
			if (duckdb_baseline) {
				duckdb::DuckDB baseline_db(duckdb_path);
				RunDuckdbBaselinePhase(baseline_db, suite, readers, duration_sec, write_interval_sec, aggregate_reader_qps,
				                       baseline);
			} else {
				RunPostgresBaselinePhase(conninfo, suite, readers, duration_sec, write_interval_sec, aggregate_reader_qps,
				                         baseline);
			}
		}

		PhaseStats accel;
		RunPostgresAccelPhase(session, suite, readers, duration_sec, write_interval_sec, aggregate_reader_qps,
		                      warmup_queries, accel);

		std::vector<double> baseline_latencies;
		std::vector<double> accel_latencies;
		if (!skip_baseline) {
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
		const double workload_speedup =
		    accel_workload_us > 0.0 && !skip_baseline ? baseline_workload_us / accel_workload_us : 0.0;
		const double baseline_p50 = Percentile(baseline_latencies, 0.50);
		const double accel_p50 = Percentile(accel_latencies, 0.50);
		const double baseline_p95 = Percentile(baseline_latencies, 0.95);
		const double accel_p95 = Percentile(accel_latencies, 0.95);
		const double read_speedup_p50 = accel_p50 > 0.0 && !skip_baseline ? baseline_p50 / accel_p50 : 0.0;
		const double read_speedup_p95 = accel_p95 > 0.0 && !skip_baseline ? baseline_p95 / accel_p95 : 0.0;
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
		std::cout << query_num << ",0," << write_interval_sec << ',' << effective_write_pct << ',' << writes_per_sec << ','
		          << readers << ',' << duration_sec << ',' << aggregate_reader_qps << ',' << reader_queries << ','
		          << baseline.writer_commits << ',' << accel.writer_commits << ',' << baseline_read_sum << ','
		          << accel_read_sum << ',' << baseline.writer_total_us << ',' << accel.writer_total_us << ','
		          << workload_speedup << ',' << read_speedup_p50 << ',' << read_speedup_p95 << ',' << baseline_p50 << ','
		          << accel_p50 << ',' << baseline_p95 << ',' << accel_p95 << ',' << accel_hit_rate << '\n';
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark_postgres_aux_workload failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
