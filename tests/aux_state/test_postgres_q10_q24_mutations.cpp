#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/partitioned_state_store.hpp"
#include "accelerator_codegen/aux_state/postgres/postgres_aux_registration.hpp"
#include "accelerator_codegen/aux_state/postgres/postgres_aux_session.hpp"
#include "accelerator_codegen/aux_state/postgres/postgres_mutation_triggers.hpp"
#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/sec_edgar_q24_aux.hpp"
#include "accelerator_codegen/sec_edgar_q24_finalize.hpp"
#include "accelerator_codegen/tpch_q10_aux.hpp"
#include "test_support/q10_aux_helpers.hpp"

#include "accelerator_codegen/sec_edgar_q24_write_policy.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "libpq-fe.h"

#if ACCEL_HAS_POSTGRES_AUX

namespace {

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

bool ValuesMatch(const std::string &actual, const std::string &expected) {
	if (actual == expected) {
		return true;
	}
	try {
		const double actual_value = std::stod(actual);
		const double expected_value = std::stod(expected);
		return std::abs(actual_value - expected_value) < 1e-6;
	} catch (...) {
		return false;
	}
}

std::string GetPostgresConninfo() {
	const char *env = std::getenv("ACCEL_POSTGRES_CONNINFO");
	if (env && *env) {
		return env;
	}
	return "dbname=accel_aux_test host=localhost port=5433 user=postgres";
}

void RequireEqualSecEdgarQ24Results(duckdb::Connection &connection, accelerator_codegen::aux_state::PostgresAuxSession &session,
                                    const std::string &label) {
	const std::string reference_sql =
	    "SELECT n.tag, n.version, COUNT(*) AS cnt, SUM(n.value) AS total "
	    "FROM num n "
	    "LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version AND n.adsh = p.adsh "
	    "WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 AND 20231231 "
	    "AND n.value IS NOT NULL AND p.adsh IS NULL "
	    "GROUP BY n.tag, n.version "
	    "HAVING COUNT(*) > 10 "
	    "ORDER BY cnt DESC "
	    "LIMIT 100";
	auto actual = connection.Query("SELECT * FROM accel_sec_edgar_q24_aux()");
	if (actual == nullptr || actual->HasError()) {
		throw std::runtime_error(label + ": aux failed: " + (actual ? actual->GetError() : "null"));
	}
	auto &actual_mat = actual->Cast<duckdb::MaterializedQueryResult>();

	PGresult *expected_pg = PQexec(&session.Connection(), reference_sql.c_str());
	if (expected_pg == nullptr || PQresultStatus(expected_pg) != PGRES_TUPLES_OK) {
		std::string error = PQerrorMessage(&session.Connection());
		if (expected_pg) {
			error += PQresultErrorMessage(expected_pg);
		}
		PQclear(expected_pg);
		throw std::runtime_error(label + ": postgres reference failed: " + error);
	}
	const int expected_rows = PQntuples(expected_pg);
	if (static_cast<duckdb::idx_t>(expected_rows) != actual_mat.RowCount()) {
		PQclear(expected_pg);
		throw std::runtime_error(label + ": row count mismatch");
	}
	for (int row = 0; row < expected_rows; row++) {
		for (int col = 0; col < PQnfields(expected_pg); col++) {
			const char *pg_value = PQgetisnull(expected_pg, row, col) ? "NULL" : PQgetvalue(expected_pg, row, col);
			if (!ValuesMatch(actual_mat.GetValue(static_cast<duckdb::idx_t>(col), static_cast<duckdb::idx_t>(row)).ToString(),
			                 pg_value)) {
				PQclear(expected_pg);
				throw std::runtime_error(label + ": mismatch at row " + std::to_string(row));
			}
		}
	}
	PQclear(expected_pg);
}

std::string PostgresReferenceQ10Sql(const std::string &start_date) {
	return "SELECT c_custkey, c_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue, c_acctbal, n_name, "
	       "c_address, c_phone, c_comment "
	       "FROM customer, orders, lineitem, nation "
	       "WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND c_nationkey = n_nationkey "
	       "AND o_orderdate >= DATE '" +
	       start_date + "' AND o_orderdate < DATE '" + start_date + "' + interval '3' month AND l_returnflag = 'R' "
	       "GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, c_comment "
	       "ORDER BY revenue DESC, c_custkey";
}

void RequireEqualQ10ResultsPostgres(accelerator_codegen::aux_state::PostgresAuxSession &session, duckdb::Connection &connection,
                                    const std::string &label, const std::string &start_date) {
	const std::string reference_sql = PostgresReferenceQ10Sql(start_date);
	const std::string aux_sql = accelerator_codegen::test_support::AcceleratorQ10AuxSql(start_date);

	auto actual = connection.Query(aux_sql);
	if (actual == nullptr || actual->HasError()) {
		throw std::runtime_error(label + ": accelerator failed: " + (actual ? actual->GetError() : "null"));
	}
	auto &actual_mat = actual->Cast<duckdb::MaterializedQueryResult>();

	PGresult *expected_pg = PQexec(&session.Connection(), reference_sql.c_str());
	if (expected_pg == nullptr || PQresultStatus(expected_pg) != PGRES_TUPLES_OK) {
		std::string error = PQerrorMessage(&session.Connection());
		if (expected_pg) {
			error += PQresultErrorMessage(expected_pg);
		}
		PQclear(expected_pg);
		throw std::runtime_error(label + ": postgres reference failed: " + error);
	}
	const int expected_rows = PQntuples(expected_pg);
	if (static_cast<duckdb::idx_t>(expected_rows) != actual_mat.RowCount()) {
		PQclear(expected_pg);
		throw std::runtime_error(label + ": row counts differ");
	}
	for (int row = 0; row < expected_rows; row++) {
		for (int col = 0; col < PQnfields(expected_pg); col++) {
			const char *pg_value = PQgetisnull(expected_pg, row, col) ? "NULL" : PQgetvalue(expected_pg, row, col);
			if (!ValuesMatch(actual_mat.GetValue(static_cast<duckdb::idx_t>(col), static_cast<duckdb::idx_t>(row)).ToString(),
			                 pg_value)) {
				PQclear(expected_pg);
				throw std::runtime_error(label + ": mismatch at row " + std::to_string(row) + ", col " + std::to_string(col));
			}
		}
	}
	PQclear(expected_pg);
}

void CreateQ10Schema(accelerator_codegen::aux_state::PostgresAuxSession &session) {
	session.Execute("DROP TABLE IF EXISTS lineitem");
	session.Execute("DROP TABLE IF EXISTS orders");
	session.Execute("DROP TABLE IF EXISTS customer");
	session.Execute("DROP TABLE IF EXISTS nation");
	session.Execute(
	    "CREATE TABLE nation(n_nationkey INTEGER NOT NULL, n_name VARCHAR NOT NULL, n_regionkey INTEGER NOT NULL, "
	    "n_comment VARCHAR NOT NULL)");
	session.Execute(
	    "CREATE TABLE customer(c_custkey BIGINT NOT NULL, c_name VARCHAR NOT NULL, c_address VARCHAR NOT NULL, "
	    "c_nationkey INTEGER NOT NULL, c_phone VARCHAR NOT NULL, c_acctbal DECIMAL(15,2) NOT NULL, "
	    "c_mktsegment VARCHAR NOT NULL, c_comment VARCHAR NOT NULL)");
	session.Execute(
	    "CREATE TABLE orders(o_orderkey BIGINT NOT NULL, o_custkey BIGINT NOT NULL, o_orderstatus VARCHAR NOT NULL, "
	    "o_totalprice DECIMAL(15,2) NOT NULL, o_orderdate DATE NOT NULL, o_orderpriority VARCHAR NOT NULL, "
	    "o_clerk VARCHAR NOT NULL, o_shippriority INTEGER NOT NULL, o_comment VARCHAR NOT NULL)");
	session.Execute(
	    "CREATE TABLE lineitem(l_orderkey BIGINT NOT NULL, l_partkey BIGINT NOT NULL, l_suppkey BIGINT NOT NULL, "
	    "l_linenumber BIGINT NOT NULL, l_quantity DECIMAL(15,2) NOT NULL, l_extendedprice DECIMAL(15,2) NOT NULL, "
	    "l_discount DECIMAL(15,2) NOT NULL, l_tax DECIMAL(15,2) NOT NULL, l_returnflag VARCHAR NOT NULL, "
	    "l_linestatus VARCHAR NOT NULL, l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL, "
	    "l_receiptdate DATE NOT NULL, l_shipinstruct VARCHAR NOT NULL, l_shipmode VARCHAR NOT NULL, "
	    "l_comment VARCHAR NOT NULL)");
	session.Commit();
}

void SeedQ10(accelerator_codegen::aux_state::PostgresAuxSession &session) {
	session.Execute("DELETE FROM lineitem");
	session.Execute("DELETE FROM orders");
	session.Execute("DELETE FROM customer");
	session.Execute("DELETE FROM nation");
	session.Execute("INSERT INTO nation VALUES (0, 'NATION0', 0, 'n')");
	session.Execute("INSERT INTO customer VALUES (1, 'C1', 'ADDR', 0, 'PHONE', 1000.00, 'BUILDING', 'c')");
	session.Execute("INSERT INTO orders VALUES (100, 1, 'O', 1000.00, DATE '1993-06-15', '1-URGENT', 'c', 0, 'c')");
	session.Execute("INSERT INTO lineitem VALUES (100, 1, 1, 1, 1.00, 100.00, 0.00, 0.00, 'R', 'O', "
	                  "DATE '1993-06-10', DATE '1993-06-15', DATE '1993-06-20', 'DELIVER IN PERSON', 'MAIL', 'c')");
	session.Commit();
}

void CreateQ24Schema(accelerator_codegen::aux_state::PostgresAuxSession &session) {
	session.Execute("DROP TABLE IF EXISTS num");
	session.Execute("DROP TABLE IF EXISTS pre");
	session.Execute("CREATE TABLE pre(adsh VARCHAR, tag VARCHAR, version VARCHAR)");
	session.Execute(
	    "CREATE TABLE num(adsh VARCHAR, tag VARCHAR, version VARCHAR, uom VARCHAR, value DOUBLE PRECISION, ddate INTEGER)");
	session.Commit();
}

void SeedQ24(accelerator_codegen::aux_state::PostgresAuxSession &session) {
	session.Execute("DELETE FROM num");
	session.Execute("DELETE FROM pre");
	session.Execute("INSERT INTO pre (adsh, tag, version) VALUES ('0000000001-01-000001', 'Revenue', '1')");
	for (int i = 0; i < 12; i++) {
		session.Execute("INSERT INTO num (adsh, tag, version, uom, value, ddate) VALUES ('0000000002-01-00000" +
		                std::to_string(i + 1) + "', 'Revenue', '1', 'USD', " + std::to_string(10.0 + i) + ", 20230601)");
	}
	session.Commit();
}

void SetupQ10ScanBridge(accelerator_codegen::aux_state::PostgresAuxSession &session) {
	session.EnsureScanBridge();
	auto &connection = session.ScanConnection();
	accelerator_codegen::RegisterAcceleratorFunctions(connection);
	connection.Query("PRAGMA threads=1");
	session.WithMutationContextTransaction(accelerator_codegen::BuildQ10AuxState);
}

void TestQ10PostgresTriggers() {
	accelerator_codegen::test_support::EnableHybridMaintenance();
	accelerator_codegen::aux_state::PostgresConnectionConfig config;
	config.conninfo = GetPostgresConninfo();
	accelerator_codegen::aux_state::PostgresAuxSession session(config);
	CreateQ10Schema(session);
	SeedQ10(session);

	accelerator_codegen::RegisterQ10AuxAcceleratorPostgres(session);
	accelerator_codegen::aux_state::PostgresQ10TxnLifecycleBackend backend;
	backend.Install(session);

	SetupQ10ScanBridge(session);
	auto &connection = session.ScanConnection();
	RequireEqualQ10ResultsPostgres(session, connection, "baseline", "1993-06-01");

	session.BeginTransaction();
	session.Execute("INSERT INTO lineitem VALUES (100, 1, 1, 2, 1.00, 50.00, 0.00, 0.00, 'R', 'O', "
	                  "DATE '1993-06-10', DATE '1993-06-15', DATE '1993-06-25', 'DELIVER IN PERSON', 'MAIL', 'c')");
	session.Commit();

	RequireEqualQ10ResultsPostgres(session, connection, "after_lineitem_insert", "1993-06-01");
	Require(accelerator_codegen::GetQ10AuxDiagnostics().used_aux_path, "expected aux fast path after incremental maintenance");
	accelerator_codegen::test_support::RequireNoIncrementalFailures("q10_postgres");

	auto &store = session.Registry().GetStore(accelerator_codegen::kQ10AuxAcceleratorId);
	const auto partition = accelerator_codegen::PartitionKeyFromOrderdate(duckdb::Date::FromString("1993-06-15"));
	Require(store.GetStatus(partition) == accelerator_codegen::aux_state::PartitionStatus::Valid,
	        "q10 partition valid after incremental maintenance");
}

void SetupQ24ScanBridge(accelerator_codegen::aux_state::PostgresAuxSession &session) {
	session.EnsureScanBridge();
	auto &connection = session.ScanConnection();
	accelerator_codegen::RegisterAcceleratorFunctions(connection);
	connection.Query("PRAGMA threads=1");
	session.WithMutationContextTransaction(accelerator_codegen::BuildSecEdgarQ24AuxState);
}

void TestQ24PostgresTriggers() {
	accelerator_codegen::aux_state::PostgresConnectionConfig config;
	config.conninfo = GetPostgresConninfo();
	accelerator_codegen::aux_state::PostgresAuxSession session(config);
	CreateQ24Schema(session);
	SeedQ24(session);

	accelerator_codegen::RegisterSecEdgarQ24AuxAcceleratorPostgres(session);
	accelerator_codegen::aux_state::PostgresQ24TxnLifecycleBackend backend;
	backend.Install(session);

	SetupQ24ScanBridge(session);
	auto &connection = session.ScanConnection();
	RequireEqualSecEdgarQ24Results(connection, session, "baseline");

	session.BeginTransaction();
	session.Execute(
	    "INSERT INTO num (adsh, tag, version, uom, value, ddate) VALUES ('0000000002-01-000013', 'Revenue', '1', 'USD', 5.0, 20230601)");
	session.Commit();

	RequireEqualSecEdgarQ24Results(connection, session, "after_num_insert");
	Require(accelerator_codegen::GetSecEdgarQ24AuxCounters().incremental_apply_failures == 0,
	        "q24_postgres: expected zero incremental_apply_failures");

	auto &store = session.Registry().GetStore(accelerator_codegen::kSecEdgarQ24AuxAcceleratorId);
	Require(store.GetStatus(accelerator_codegen::kSecEdgarQ24GroupAggregatesSlot) ==
	            accelerator_codegen::aux_state::PartitionStatus::Valid,
	        "q24 group aggregates slot valid after incremental maintenance");
	Require(store.GetStatus(accelerator_codegen::kSecEdgarQ24OutputSlot) ==
	            accelerator_codegen::aux_state::PartitionStatus::Valid,
	        "q24 output slot valid after incremental maintenance");
}

} // namespace

int main() {
	try {
		if (std::getenv("ACCEL_POSTGRES_SKIP") != nullptr) {
			std::cout << "test_postgres_aux_mutations skipped (ACCEL_POSTGRES_SKIP)\n";
			return EXIT_SUCCESS;
		}
		TestQ10PostgresTriggers();
		TestQ24PostgresTriggers();
		std::cout << "test_postgres_aux_mutations passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_postgres_aux_mutations failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}

#else

int main() {
	std::cout << "test_postgres_aux_mutations skipped (PostgreSQL not found at configure time)\n";
	return EXIT_SUCCESS;
}

#endif
