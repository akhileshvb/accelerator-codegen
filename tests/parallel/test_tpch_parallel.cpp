#include "accelerator_codegen/register.hpp"
#include "test_support/q1_manifest.hpp"
#include "test_support/q3_manifest.hpp"
#include "test_support/q4_manifest.hpp"
#include "test_support/q6_manifest.hpp"
#include "test_support/q9_manifest.hpp"
#include "test_support/q12_manifest.hpp"
#include "test_support/q14_manifest.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#ifndef ACCEL_ENABLE_PARALLEL_TPCH
#error "test_tpch_parallel requires -DACCEL_ENABLE_PARALLEL_TPCH"
#endif

namespace {

constexpr const char *kDefaultSf1Db = "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
constexpr const char *kDefaultSf10Db = "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf10.duckdb";

std::string GetTpchDbPath() {
	const char *env_path = std::getenv("ACCEL_TPCH_DB");
	if (env_path && *env_path) {
		return env_path;
	}
	return kDefaultSf1Db;
}

bool FileExists(const std::string &path) {
	std::ifstream in(path);
	return in.good();
}

bool PathLooksLikeSf10(const std::string &path) {
	return path.find("sf10") != std::string::npos || path.find("SF10") != std::string::npos;
}

bool AccelParallelSf10Enabled() {
	const char *env = std::getenv("ACCEL_PARALLEL_SF10");
	if (env && *env) {
		return std::string(env) != "0";
	}
	// Default ON when the default SF10 file is present.
	return FileExists(kDefaultSf10Db);
}

bool ShouldRunSf10Suite() {
	if (PathLooksLikeSf10(GetTpchDbPath())) {
		return true;
	}
	return FileExists(kDefaultSf10Db) && AccelParallelSf10Enabled();
}

std::string GetSf10DbPath() {
	const auto env_path = GetTpchDbPath();
	if (PathLooksLikeSf10(env_path)) {
		return env_path;
	}
	return kDefaultSf10Db;
}

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

void RequireEqualResults(duckdb::MaterializedQueryResult &lhs, duckdb::MaterializedQueryResult &rhs,
                         const std::string &label) {
	Require(!lhs.HasError(), label + ": lhs error: " + lhs.GetError());
	Require(!rhs.HasError(), label + ": rhs error: " + rhs.GetError());
	Require(lhs.RowCount() == rhs.RowCount(), label + ": row count differs");
	Require(lhs.ColumnCount() == rhs.ColumnCount(), label + ": column count differs");
	for (duckdb::idx_t row = 0; row < lhs.RowCount(); row++) {
		for (duckdb::idx_t col = 0; col < lhs.ColumnCount(); col++) {
			const auto lv = lhs.GetValue(col, row);
			const auto rv = rhs.GetValue(col, row);
			Require(lv.IsNull() == rv.IsNull(), label + ": nullability differs at row " + std::to_string(row) +
			                                        " col " + std::to_string(col));
			if (lv.IsNull()) {
				continue;
			}
			Require(lv.type() == rv.type(), label + ": type differs at row " + std::to_string(row));
			Require(lv == rv, label + ": mismatch at row " + std::to_string(row) + " col " + std::to_string(col));
		}
	}
}

void SetThreads(duckdb::Connection &connection, int threads) {
	auto result = connection.Query("PRAGMA threads=" + std::to_string(threads));
	Require(result && !result->HasError(), "failed to set threads=" + std::to_string(threads));
	auto check = connection.Query("SELECT current_setting('threads')");
	Require(check && !check->HasError(), "failed to read threads setting");
	const auto observed =
	    check->Cast<duckdb::MaterializedQueryResult>().GetValue(0, 0).GetValue<int64_t>();
	Require(observed == threads, "expected threads=" + std::to_string(threads) + " got " + std::to_string(observed));
}

void CompareTriple(duckdb::Connection &connection, const std::string &label, const std::string &duckdb_sql,
                   const std::string &single_sql, const std::string &parallel_sql) {
	auto duck = connection.Query(duckdb_sql);
	auto single = connection.Query(single_sql);
	auto parallel = connection.Query(parallel_sql);
	Require(duck && single && parallel, label + ": null query result");
	auto &d = duck->Cast<duckdb::MaterializedQueryResult>();
	auto &s = single->Cast<duckdb::MaterializedQueryResult>();
	auto &p = parallel->Cast<duckdb::MaterializedQueryResult>();
	RequireEqualResults(d, s, label + " duckdb vs single");
	RequireEqualResults(s, p, label + " single vs parallel");
	auto parallel2 = connection.Query(parallel_sql);
	Require(parallel2 != nullptr, label + ": null parallel repeat");
	RequireEqualResults(p, parallel2->Cast<duckdb::MaterializedQueryResult>(), label + " parallel determinism");
}

void CompareSingleVsParallel(duckdb::Connection &connection, const std::string &label,
                             const std::string &single_sql, const std::string &parallel_sql) {
	auto single = connection.Query(single_sql);
	auto parallel = connection.Query(parallel_sql);
	Require(single && parallel, label + ": null query result");
	RequireEqualResults(single->Cast<duckdb::MaterializedQueryResult>(),
	                    parallel->Cast<duckdb::MaterializedQueryResult>(), label);
}

std::string EscapeLiteral(const std::string &value) {
	std::string out;
	out.reserve(value.size());
	for (char c : value) {
		if (c == '\'') {
			out += "''";
		} else {
			out.push_back(c);
		}
	}
	return out;
}

void RunSf1Suite(duckdb::Connection &connection, const std::vector<int> &thread_counts) {
	const int seed_limit = 20;

	for (int threads : thread_counts) {
		SetThreads(connection, threads);
		std::cerr << "sf1 threads=" << threads << "\n";

		for (int i = 0; i < seed_limit && i < static_cast<int>(accelerator_codegen::test_support::kQ6Manifest.size());
		     i++) {
			const auto &e = accelerator_codegen::test_support::kQ6Manifest[i];
			const auto duck_sql =
			    std::string("SELECT sum(l_extendedprice * l_discount)::DECIMAL(38,4) AS revenue FROM lineitem "
			                "WHERE l_shipdate >= DATE '") +
			    e.start_date + "' AND l_shipdate < DATE '" + e.start_date +
			    "' + INTERVAL '1' YEAR AND l_discount BETWEEN " + e.discount +
			    " - 0.01 AND " + e.discount + " + 0.01 AND l_quantity < " + e.quantity;
			const auto single_sql = std::string("SELECT revenue FROM accel_tpch_q6(DATE '") + e.start_date +
			                        "', " + e.discount + ", " + e.quantity + ")";
			const auto parallel_sql = std::string("SELECT revenue FROM accel_tpch_q6_parallel(DATE '") +
			                          e.start_date + "', " + e.discount + ", " + e.quantity + ")";
			CompareTriple(connection, std::string(e.label) + " t=" + std::to_string(threads), duck_sql, single_sql,
			              parallel_sql);
		}

		for (int i = 0; i < seed_limit && i < static_cast<int>(accelerator_codegen::test_support::kQ1Manifest.size());
		     i++) {
			const auto &e = accelerator_codegen::test_support::kQ1Manifest[i];
			const auto single_sql = "SELECT * FROM accel_tpch_q1(" + std::to_string(e.day_offset) + ")";
			const auto parallel_sql = "SELECT * FROM accel_tpch_q1_parallel(" + std::to_string(e.day_offset) + ")";
			CompareSingleVsParallel(connection, std::string(e.label) + " t=" + std::to_string(threads), single_sql,
			                        parallel_sql);
		}

		for (int i = 0; i < 5 && i < static_cast<int>(accelerator_codegen::test_support::kQ3Manifest.size()); i++) {
			const auto &e = accelerator_codegen::test_support::kQ3Manifest[i];
			const auto single_sql = std::string("SELECT * FROM accel_tpch_q3('") + EscapeLiteral(e.market_segment) +
			                        "', DATE '" + e.cutoff_date + "')";
			const auto parallel_sql = std::string("SELECT * FROM accel_tpch_q3_parallel('") +
			                          EscapeLiteral(e.market_segment) + "', DATE '" + e.cutoff_date + "')";
			CompareSingleVsParallel(connection, std::string(e.label) + " t=" + std::to_string(threads), single_sql,
			                        parallel_sql);
		}

		for (int i = 0; i < 5 && i < static_cast<int>(accelerator_codegen::test_support::kQ4Manifest.size()); i++) {
			const auto &e = accelerator_codegen::test_support::kQ4Manifest[i];
			const auto single_sql = std::string("SELECT * FROM accel_tpch_q4(DATE '") + e.start_date + "')";
			const auto parallel_sql =
			    std::string("SELECT * FROM accel_tpch_q4_parallel(DATE '") + e.start_date + "')";
			CompareSingleVsParallel(connection, std::string(e.label) + " t=" + std::to_string(threads), single_sql,
			                        parallel_sql);
		}

		for (int i = 0; i < 5 && i < static_cast<int>(accelerator_codegen::test_support::kQ9Manifest.size()); i++) {
			const auto &e = accelerator_codegen::test_support::kQ9Manifest[i];
			const auto single_sql = std::string("SELECT * FROM accel_tpch_q9('") + EscapeLiteral(e.color) + "')";
			const auto parallel_sql =
			    std::string("SELECT * FROM accel_tpch_q9_parallel('") + EscapeLiteral(e.color) + "')";
			CompareSingleVsParallel(connection, std::string(e.label) + " t=" + std::to_string(threads), single_sql,
			                        parallel_sql);
		}

		for (int i = 0; i < 5 && i < static_cast<int>(accelerator_codegen::test_support::kQ12Manifest.size()); i++) {
			const auto &e = accelerator_codegen::test_support::kQ12Manifest[i];
			const auto single_sql = std::string("SELECT * FROM accel_tpch_q12('") + EscapeLiteral(e.shipmode1) +
			                        "', '" + EscapeLiteral(e.shipmode2) + "', DATE '" + e.start_date + "')";
			const auto parallel_sql = std::string("SELECT * FROM accel_tpch_q12_parallel('") +
			                          EscapeLiteral(e.shipmode1) + "', '" + EscapeLiteral(e.shipmode2) +
			                          "', DATE '" + e.start_date + "')";
			CompareSingleVsParallel(connection, std::string(e.label) + " t=" + std::to_string(threads), single_sql,
			                        parallel_sql);
		}

		for (int i = 0; i < 5 && i < static_cast<int>(accelerator_codegen::test_support::kQ14Manifest.size()); i++) {
			const auto &e = accelerator_codegen::test_support::kQ14Manifest[i];
			const auto single_sql = std::string("SELECT * FROM accel_tpch_q14(DATE '") + e.start_date + "')";
			const auto parallel_sql =
			    std::string("SELECT * FROM accel_tpch_q14_parallel(DATE '") + e.start_date + "')";
			CompareSingleVsParallel(connection, std::string(e.label) + " t=" + std::to_string(threads), single_sql,
			                        parallel_sql);
		}
	}
}

void RunSf10Suite(duckdb::Connection &connection) {
	const std::vector<int> thread_counts = {1, 4, 8};
	Require(!accelerator_codegen::test_support::kQ6Manifest.empty(), "empty Q6 manifest");
	Require(!accelerator_codegen::test_support::kQ1Manifest.empty(), "empty Q1 manifest");
	Require(!accelerator_codegen::test_support::kQ3Manifest.empty(), "empty Q3 manifest");
	Require(!accelerator_codegen::test_support::kQ4Manifest.empty(), "empty Q4 manifest");
	Require(!accelerator_codegen::test_support::kQ9Manifest.empty(), "empty Q9 manifest");
	Require(!accelerator_codegen::test_support::kQ12Manifest.empty(), "empty Q12 manifest");
	Require(!accelerator_codegen::test_support::kQ14Manifest.empty(), "empty Q14 manifest");

	const auto &q6 = accelerator_codegen::test_support::kQ6Manifest[0];
	const auto &q1 = accelerator_codegen::test_support::kQ1Manifest[0];
	const auto &q3 = accelerator_codegen::test_support::kQ3Manifest[0];
	const auto &q4 = accelerator_codegen::test_support::kQ4Manifest[0];
	const auto &q9 = accelerator_codegen::test_support::kQ9Manifest[0];
	const auto &q12 = accelerator_codegen::test_support::kQ12Manifest[0];
	const auto &q14 = accelerator_codegen::test_support::kQ14Manifest[0];

	for (int threads : thread_counts) {
		SetThreads(connection, threads);
		std::cerr << "sf10 threads=" << threads << "\n";
		const auto tlabel = std::string(" t=") + std::to_string(threads);

		CompareSingleVsParallel(
		    connection, std::string(q6.label) + " sf10" + tlabel,
		    std::string("SELECT revenue FROM accel_tpch_q6(DATE '") + q6.start_date + "', " + q6.discount + ", " +
		        q6.quantity + ")",
		    std::string("SELECT revenue FROM accel_tpch_q6_parallel(DATE '") + q6.start_date + "', " + q6.discount +
		        ", " + q6.quantity + ")");

		CompareSingleVsParallel(connection, std::string(q1.label) + " sf10" + tlabel,
		                        "SELECT * FROM accel_tpch_q1(" + std::to_string(q1.day_offset) + ")",
		                        "SELECT * FROM accel_tpch_q1_parallel(" + std::to_string(q1.day_offset) + ")");

		CompareSingleVsParallel(connection, std::string(q3.label) + " sf10" + tlabel,
		                        std::string("SELECT * FROM accel_tpch_q3('") + EscapeLiteral(q3.market_segment) +
		                            "', DATE '" + q3.cutoff_date + "')",
		                        std::string("SELECT * FROM accel_tpch_q3_parallel('") +
		                            EscapeLiteral(q3.market_segment) + "', DATE '" + q3.cutoff_date + "')");

		CompareSingleVsParallel(connection, std::string(q4.label) + " sf10" + tlabel,
		                        std::string("SELECT * FROM accel_tpch_q4(DATE '") + q4.start_date + "')",
		                        std::string("SELECT * FROM accel_tpch_q4_parallel(DATE '") + q4.start_date + "')");

		CompareSingleVsParallel(connection, std::string(q9.label) + " sf10" + tlabel,
		                        std::string("SELECT * FROM accel_tpch_q9('") + EscapeLiteral(q9.color) + "')",
		                        std::string("SELECT * FROM accel_tpch_q9_parallel('") + EscapeLiteral(q9.color) +
		                            "')");

		CompareSingleVsParallel(connection, std::string(q12.label) + " sf10" + tlabel,
		                        std::string("SELECT * FROM accel_tpch_q12('") + EscapeLiteral(q12.shipmode1) +
		                            "', '" + EscapeLiteral(q12.shipmode2) + "', DATE '" + q12.start_date + "')",
		                        std::string("SELECT * FROM accel_tpch_q12_parallel('") +
		                            EscapeLiteral(q12.shipmode1) + "', '" + EscapeLiteral(q12.shipmode2) +
		                            "', DATE '" + q12.start_date + "')");

		CompareSingleVsParallel(connection, std::string(q14.label) + " sf10" + tlabel,
		                        std::string("SELECT * FROM accel_tpch_q14(DATE '") + q14.start_date + "')",
		                        std::string("SELECT * FROM accel_tpch_q14_parallel(DATE '") + q14.start_date + "')");
	}
}

void RunQ6TransactionTest() {
	duckdb::DuckDB mem_db(nullptr);
	duckdb::Connection mem_con(mem_db);
	accelerator_codegen::RegisterAcceleratorFunctions(mem_con);
	SetThreads(mem_con, 4);

	auto setup = mem_con.Query(R"SQL(
		CREATE TABLE lineitem(
			l_orderkey BIGINT, l_partkey BIGINT, l_suppkey BIGINT, l_linenumber BIGINT,
			l_quantity DECIMAL(15,2), l_extendedprice DECIMAL(15,2), l_discount DECIMAL(15,2),
			l_tax DECIMAL(15,2), l_returnflag VARCHAR, l_linestatus VARCHAR,
			l_shipdate DATE, l_commitdate DATE, l_receiptdate DATE,
			l_shipinstruct VARCHAR, l_shipmode VARCHAR, l_comment VARCHAR
		);
		INSERT INTO lineitem VALUES
			(1, 10, 100, 1, 10.00, 100.00, 0.06, 0.01, 'N', 'O',
			 DATE '1994-06-01', DATE '1994-06-02', DATE '1994-06-03', 'DELIVER', 'MAIL', 'base'),
			(2, 20, 200, 1, 5.00, 50.00, 0.06, 0.01, 'N', 'O',
			 DATE '1993-01-01', DATE '1993-01-02', DATE '1993-01-03', 'DELIVER', 'MAIL', 'out_of_range');
	)SQL");
	Require(setup && !setup->HasError(), "txn setup failed: " + (setup ? setup->GetError() : "null"));

	const std::string duck_sql =
	    "SELECT sum(l_extendedprice * l_discount)::DECIMAL(38,4) AS revenue FROM lineitem "
	    "WHERE l_shipdate >= DATE '1994-01-01' AND l_shipdate < DATE '1995-01-01' "
	    "AND l_discount BETWEEN 0.05 AND 0.07 AND l_quantity < 24";
	const std::string parallel_sql =
	    "SELECT revenue FROM accel_tpch_q6_parallel(DATE '1994-01-01', 0.06, 24)";

	auto baseline_duck = mem_con.Query(duck_sql);
	auto baseline_par = mem_con.Query(parallel_sql);
	Require(baseline_duck && baseline_par, "txn baseline null");
	RequireEqualResults(baseline_duck->Cast<duckdb::MaterializedQueryResult>(),
	                    baseline_par->Cast<duckdb::MaterializedQueryResult>(), "txn baseline");

	mem_con.BeginTransaction();
	auto insert = mem_con.Query(
	    "INSERT INTO lineitem VALUES "
	    "(3, 30, 300, 1, 8.00, 200.00, 0.06, 0.01, 'N', 'O', "
	    "DATE '1994-07-15', DATE '1994-07-16', DATE '1994-07-17', 'DELIVER', 'MAIL', 'txn_insert')");
	Require(insert && !insert->HasError(), "txn insert failed");

	auto after_duck = mem_con.Query(duck_sql);
	auto after_par = mem_con.Query(parallel_sql);
	Require(after_duck && after_par, "txn after-insert null");
	RequireEqualResults(after_duck->Cast<duckdb::MaterializedQueryResult>(),
	                    after_par->Cast<duckdb::MaterializedQueryResult>(), "txn after insert");

	mem_con.Rollback();

	auto rolled_duck = mem_con.Query(duck_sql);
	auto rolled_par = mem_con.Query(parallel_sql);
	Require(rolled_duck && rolled_par, "txn after-rollback null");
	RequireEqualResults(rolled_duck->Cast<duckdb::MaterializedQueryResult>(),
	                    rolled_par->Cast<duckdb::MaterializedQueryResult>(), "txn after rollback");
	RequireEqualResults(baseline_duck->Cast<duckdb::MaterializedQueryResult>(),
	                    rolled_par->Cast<duckdb::MaterializedQueryResult>(), "txn rollback restores baseline");
}

} // namespace

int main() {
	try {
#ifndef ACCEL_ENABLE_PARALLEL_TPCH
		std::cerr << "ACCEL_ENABLE_PARALLEL_TPCH is required\n";
		return 1;
#endif
		duckdb::DuckDB database(GetTpchDbPath().c_str());
		duckdb::Connection connection(database);
		accelerator_codegen::RegisterAcceleratorFunctions(connection);

		const std::vector<int> thread_counts = {1, 2, 4, 8, 16};
		RunSf1Suite(connection, thread_counts);

		if (ShouldRunSf10Suite()) {
			const auto sf10_path = GetSf10DbPath();
			Require(FileExists(sf10_path), "SF10 suite requested but DB missing: " + sf10_path);
			std::cerr << "sf10 suite ACCEL_TPCH_DB=" << sf10_path << "\n";
			duckdb::DuckDB sf10_db(sf10_path.c_str());
			duckdb::Connection sf10_con(sf10_db);
			accelerator_codegen::RegisterAcceleratorFunctions(sf10_con);
			RunSf10Suite(sf10_con);
		} else {
			std::cerr << "sf10 suite skipped\n";
		}

		// Empty-table synthetic check on an isolated in-memory database.
		{
			duckdb::DuckDB mem_db(nullptr);
			duckdb::Connection mem_con(mem_db);
			accelerator_codegen::RegisterAcceleratorFunctions(mem_con);
			SetThreads(mem_con, 4);
			mem_con.Query(R"SQL(
				CREATE TABLE lineitem(
					l_orderkey BIGINT, l_partkey BIGINT, l_suppkey BIGINT, l_linenumber BIGINT,
					l_quantity DECIMAL(15,2), l_extendedprice DECIMAL(15,2), l_discount DECIMAL(15,2),
					l_tax DECIMAL(15,2), l_returnflag VARCHAR, l_linestatus VARCHAR,
					l_shipdate DATE, l_commitdate DATE, l_receiptdate DATE,
					l_shipinstruct VARCHAR, l_shipmode VARCHAR, l_comment VARCHAR
				);
			)SQL");
			auto duck = mem_con.Query(
			    "SELECT sum(l_extendedprice * l_discount)::DECIMAL(38,4) AS revenue FROM lineitem "
			    "WHERE l_shipdate >= DATE '1994-01-01' AND l_shipdate < DATE '1995-01-01' "
			    "AND l_discount BETWEEN 0.05 AND 0.07 AND l_quantity < 24");
			auto par = mem_con.Query("SELECT revenue FROM accel_tpch_q6_parallel(DATE '1994-01-01', 0.06, 24)");
			Require(duck && par && !duck->HasError() && !par->HasError(), "empty Q6 failed");
			RequireEqualResults(duck->Cast<duckdb::MaterializedQueryResult>(),
			                    par->Cast<duckdb::MaterializedQueryResult>(), "empty Q6");
		}

		RunQ6TransactionTest();

		std::cout << "parallel TPC-H correctness OK\n";
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << "parallel test failed: " << ex.what() << "\n";
		return 1;
	}
}
