#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q14_aux.hpp"
#include "test_support/q14_aux_helpers.hpp"
#include "test_support/q14_manifest.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "duckdb.hpp"

namespace {

std::string GetTpchDbPath() {
	const char *env_path = std::getenv("ACCEL_TPCH_DB");
	if (env_path && *env_path) {
		return env_path;
	}
	return "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
}

void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

std::unique_ptr<duckdb::QueryResult> RunQuery(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Require(result != nullptr, "query returned null result");
	Require(!result->HasError(), result->GetError());
	return result;
}

void CreateQ14Schema(duckdb::Connection &connection) {
	auto result = connection.Query(
	    "CREATE TABLE part("
	    "p_partkey BIGINT NOT NULL, p_name VARCHAR NOT NULL, p_mfgr VARCHAR NOT NULL, "
	    "p_brand VARCHAR NOT NULL, p_type VARCHAR, p_size INTEGER NOT NULL, "
	    "p_container VARCHAR NOT NULL, p_retailprice DECIMAL(15,2) NOT NULL, p_comment VARCHAR NOT NULL);"
	    "CREATE TABLE lineitem("
	    "l_orderkey BIGINT NOT NULL, l_partkey BIGINT NOT NULL, l_suppkey BIGINT NOT NULL, "
	    "l_linenumber BIGINT NOT NULL, l_quantity DECIMAL(15,2) NOT NULL, "
	    "l_extendedprice DECIMAL(15,2) NOT NULL, l_discount DECIMAL(15,2) NOT NULL, "
	    "l_tax DECIMAL(15,2) NOT NULL, l_returnflag VARCHAR NOT NULL, l_linestatus VARCHAR NOT NULL, "
	    "l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL, l_receiptdate DATE NOT NULL, "
	    "l_shipinstruct VARCHAR NOT NULL, l_shipmode VARCHAR NOT NULL, l_comment VARCHAR NOT NULL);");
	Require(result != nullptr && !result->HasError(), "failed to create q14 schema: " + result->GetError());
}

void InsertPart(duckdb::Connection &connection, int64_t partkey, const std::string &type) {
	std::string type_sql = type == "NULL" ? "NULL" : ("'" + type + "'");
	auto result = connection.Query("INSERT INTO part VALUES (" + std::to_string(partkey) +
	                               ", 'name', 'Manufacturer#1', 'Brand#11', " + type_sql +
	                               ", 1, 'SM CASE', 10.00, 'comment')");
	Require(result != nullptr && !result->HasError(), "failed to insert part: " + result->GetError());
}

void InsertLineitem(duckdb::Connection &connection, int64_t orderkey, int64_t partkey, int64_t linenumber,
                    const std::string &extendedprice, const std::string &discount, const std::string &shipdate) {
	auto result = connection.Query(
	    "INSERT INTO lineitem VALUES (" + std::to_string(orderkey) + ", " + std::to_string(partkey) + ", 1, " +
	    std::to_string(linenumber) + ", 1.00, " + extendedprice + ", " + discount +
	    ", 0.00, 'N', 'O', DATE '" + shipdate + "', DATE '" + shipdate + "', DATE '" + shipdate +
	    "', 'NONE', 'AIR', 'comment')");
	Require(result != nullptr && !result->HasError(), "failed to insert lineitem: " + result->GetError());
}

void SetupSyntheticFixture(duckdb::Connection &connection) {
	CreateQ14Schema(connection);
	InsertPart(connection, 1, "PROMO BURNISHED COPPER");
	InsertPart(connection, 2, "LARGE BRUSHED BRASS");
	InsertPart(connection, 3, "promo brushed copper");
	InsertPart(connection, 4, "XPROMO STEEL");
	InsertPart(connection, 5, "PROMO");
	InsertPart(connection, 6, "NULL");
	InsertPart(connection, 7, "PROMO ANODIZED STEEL");

	InsertLineitem(connection, 1, 1, 1, "100.00", "0.00", "1995-01-01");
	InsertLineitem(connection, 2, 2, 1, "100.00", "0.00", "1995-01-15");
	InsertLineitem(connection, 3, 3, 1, "100.00", "0.00", "1995-01-15");
	InsertLineitem(connection, 4, 4, 1, "100.00", "0.00", "1995-01-15");
	InsertLineitem(connection, 5, 5, 1, "50.00", "0.10", "1995-01-31");
	InsertLineitem(connection, 6, 1, 2, "100.00", "0.00", "1995-02-01");
	InsertLineitem(connection, 7, 1, 3, "100.00", "0.00", "1994-12-31");
	InsertLineitem(connection, 8, 6, 1, "100.00", "0.00", "1995-01-15");
	InsertLineitem(connection, 9, 7, 2, "300.00", "0.00", "1995-01-21");
	InsertLineitem(connection, 10, 999, 1, "100.00", "0.00", "1995-01-15");
}

void BuildAuxState(duckdb::Connection &connection) {
	RunQuery(connection, "SELECT * FROM accel_q14_aux_build()");
}

void CompareAux(duckdb::Connection &connection, const std::string &label, const std::string &start_date) {
	accelerator_codegen::test_support::RequireEqualQ14Results(
	    connection, label, accelerator_codegen::test_support::ReferenceQ14Sql(start_date),
	    accelerator_codegen::test_support::AcceleratorQ14AuxSql(start_date));
}

void CompareAuxAndPartition(duckdb::Connection &connection, const std::string &label, const std::string &start_date,
                            bool expect_readable = true) {
	CompareAux(connection, label, start_date);
	accelerator_codegen::test_support::RequireAuxPartitionTotalsMatch(connection, label + "_partition", start_date,
	                                                                  expect_readable);
}

} // namespace

int main() {
	try {
		const bool run_sf1 = std::getenv("ACCEL_Q14_AUX_SKIP_SF1") == nullptr;
		if (run_sf1) {
			duckdb::DuckDB file_database(GetTpchDbPath().c_str());
			duckdb::Connection connection(file_database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			RunQuery(connection, "PRAGMA threads=1");

			for (const auto &entry : accelerator_codegen::test_support::kQ14Manifest) {
				CompareAux(connection, std::string(entry.label) + "_before_build", entry.start_date);
			}

			BuildAuxState(connection);

			for (const auto &entry : accelerator_codegen::test_support::kQ14Manifest) {
				CompareAuxAndPartition(connection, std::string(entry.label) + "_after_build", entry.start_date);
			}
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupSyntheticFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			BuildAuxState(connection);

			CompareAux(connection, "synthetic_mixed", "1995-01-01");
			CompareAux(connection, "synthetic_empty_window", "1990-01-01");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			CreateQ14Schema(connection);
			InsertPart(connection, 1, "PROMO STEEL");
			InsertLineitem(connection, 1, 1, 1, "100.00", "0.00", "1994-01-01");
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			BuildAuxState(connection);
			CompareAux(connection, "empty_result_null", "1995-01-01");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			CreateQ14Schema(connection);
			InsertPart(connection, 1, "PROMO STEEL");
			InsertPart(connection, 2, "LARGE STEEL");
			InsertLineitem(connection, 1, 1, 1, "0.00", "0.00", "1995-01-15");
			InsertLineitem(connection, 2, 2, 1, "0.00", "0.00", "1995-01-15");
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			BuildAuxState(connection);
			CompareAux(connection, "zero_discounted_revenue", "1995-01-01");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection writer(memory_database);
			duckdb::Connection reader(memory_database);
			CreateQ14Schema(writer);
			InsertPart(writer, 10, "PROMO STEEL");
			InsertLineitem(writer, 10, 10, 1, "100.00", "0.00", "1995-01-15");
			accelerator_codegen::RegisterAcceleratorFunctions(writer);
			accelerator_codegen::RegisterAcceleratorFunctions(reader);
			BuildAuxState(writer);

			CompareAux(writer, "transaction_baseline", "1995-01-01");

			writer.BeginTransaction();
			InsertPart(writer, 20, "PROMO COPPER");
			InsertLineitem(writer, 20, 20, 1, "200.00", "0.00", "1995-01-15");
			CompareAux(writer, "ryow_writer_sees_uncommitted", "1995-01-01");
			CompareAux(reader, "ryow_reader_still_valid", "1995-01-01");
			writer.Commit();

			CompareAux(writer, "after_commit_writer_fallback_until_rebuild", "1995-01-01");
			CompareAux(reader, "after_commit_reader_fallback", "1995-01-01");

			RunQuery(writer, "SELECT * FROM accel_q14_aux_rebuild_partition(DATE '1995-01-01')");
			CompareAux(writer, "after_partition_rebuild", "1995-01-01");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			CreateQ14Schema(connection);
			InsertPart(connection, 10, "PROMO STEEL");
			InsertLineitem(connection, 10, 10, 1, "100.00", "0.00", "1995-01-15");
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			BuildAuxState(connection);

			connection.BeginTransaction();
			InsertLineitem(connection, 11, 10, 2, "50.00", "0.00", "1995-01-20");
			CompareAux(connection, "rollback_bypass", "1995-01-01");
			connection.Rollback();
			CompareAux(connection, "after_rollback_restored", "1995-01-01");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			CreateQ14Schema(connection);
			InsertPart(connection, 1, "PROMO STEEL");
			InsertLineitem(connection, 1, 1, 1, "100.00", "0.00", "1995-01-15");
			InsertLineitem(connection, 2, 1, 2, "100.00", "0.00", "1995-02-15");
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			BuildAuxState(connection);

			RunQuery(connection, "UPDATE lineitem SET l_shipdate = DATE '1995-02-20' WHERE l_orderkey = 1");
			CompareAuxAndPartition(connection, "shipdate_move_incremental", "1995-01-01");
			CompareAuxAndPartition(connection, "shipdate_move_dest_month", "1995-02-01");
			Require(accelerator_codegen::GetQ14AuxDiagnostics().used_aux_path,
			        "expected aux fast path after shipdate move");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			CreateQ14Schema(connection);
			InsertPart(connection, 1, "PROMO STEEL");
			InsertPart(connection, 2, "LARGE STEEL");
			InsertLineitem(connection, 1, 1, 1, "100.00", "0.00", "1995-01-15");
			InsertLineitem(connection, 2, 2, 1, "200.00", "0.00", "1995-01-15");
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			BuildAuxState(connection);
			CompareAuxAndPartition(connection, "incremental_baseline", "1995-01-01");

			RunQuery(connection, "INSERT INTO lineitem VALUES (3, 1, 1, 3, 1.00, 50.00, 0.00, "
			                     "0.00, 'N', 'O', DATE '1995-01-20', DATE '1995-01-20', DATE '1995-01-20', "
			                     "'NONE', 'AIR', 'comment')");
			CompareAuxAndPartition(connection, "incremental_after_lineitem_insert", "1995-01-01");
			Require(accelerator_codegen::GetQ14AuxDiagnostics().used_aux_path,
			        "expected aux fast path after incremental lineitem insert");

			RunQuery(connection, "UPDATE lineitem SET l_extendedprice = 75.00, l_discount = 0.00 WHERE l_orderkey = 3");
			CompareAuxAndPartition(connection, "incremental_after_lineitem_update", "1995-01-01");
			Require(accelerator_codegen::GetQ14AuxDiagnostics().used_aux_path,
			        "expected aux fast path after incremental lineitem update");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			CreateQ14Schema(connection);
			InsertPart(connection, 1, "LARGE STEEL");
			InsertLineitem(connection, 1, 1, 1, "100.00", "0.00", "1995-01-15");
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			BuildAuxState(connection);

			RunQuery(connection, "UPDATE part SET p_type = 'PROMO STEEL' WHERE p_partkey = 1");
			CompareAuxAndPartition(connection, "part_type_change_incremental", "1995-01-01");
			Require(accelerator_codegen::GetQ14AuxDiagnostics().used_aux_path,
			        "expected aux fast path after incremental part p_type flip");
		}

		std::cout << "test_tpch_q14_aux passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "test_tpch_q14_aux failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
