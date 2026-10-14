#include "backends/duckdb/api/register.hpp"
#include "tpch/q9/tests/q9_manifest.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

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

std::string ReadFile(const std::string &path) {
	std::ifstream input(path);
	Require(input.good(), "failed to open file: " + path);
	std::ostringstream buffer;
	buffer << input.rdbuf();
	return buffer.str();
}

std::unique_ptr<duckdb::QueryResult> RunQuery(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Require(result != nullptr, "query returned null result");
	Require(!result->HasError(), result->GetError());
	return result;
}

std::vector<std::string> MaterializeOrderedRows(duckdb::MaterializedQueryResult &result) {
	std::vector<std::string> rows;
	rows.reserve(result.RowCount());
	for (duckdb::idx_t row = 0; row < result.RowCount(); row++) {
		std::string encoded;
		for (duckdb::idx_t col = 0; col < result.ColumnCount(); col++) {
			if (col != 0) {
				encoded += " | ";
			}
			encoded += result.GetValue(col, row).ToString();
		}
		rows.push_back(std::move(encoded));
	}
	return rows;
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

std::string ReferenceQ9Sql(const std::string &color) {
	const auto escaped = EscapeSqlLiteral(color);
	return "SELECT nation, o_year, SUM(amount) AS sum_profit FROM ("
	       "SELECT n_name AS nation, EXTRACT(year FROM o_orderdate) AS o_year, "
	       "l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity AS amount "
	       "FROM part, supplier, lineitem, partsupp, orders, nation "
	       "WHERE s_suppkey = l_suppkey "
	       "AND ps_suppkey = l_suppkey "
	       "AND ps_partkey = l_partkey "
	       "AND p_partkey = l_partkey "
	       "AND o_orderkey = l_orderkey "
	       "AND s_nationkey = n_nationkey "
	       "AND p_name LIKE '%' || '" +
	       escaped +
	       "' || '%' "
	       ") AS profit "
	       "GROUP BY nation, o_year "
	       "ORDER BY nation, o_year DESC";
}

std::string AcceleratorQ9Sql(const std::string &color) {
	return "SELECT * FROM accel_tpch_q9('" + EscapeSqlLiteral(color) + "')";
}

std::string StripTrailingSemicolon(std::string sql) {
	while (!sql.empty() && (std::isspace(static_cast<unsigned char>(sql.back())) || sql.back() == ';')) {
		sql.pop_back();
	}
	return sql;
}

void RequireEqualQ9Results(duckdb::Connection &connection, const std::string &label, const std::string &reference_sql,
                           const std::string &accelerator_sql) {
	auto expected = RunQuery(connection, reference_sql);
	auto actual = RunQuery(connection, accelerator_sql);

	auto &expected_materialized = expected->Cast<duckdb::MaterializedQueryResult>();
	auto &actual_materialized = actual->Cast<duckdb::MaterializedQueryResult>();

	Require(expected->names == actual->names, label + ": column names differ");
	Require(expected->types == actual->types, label + ": column types differ");
	Require(expected_materialized.RowCount() == actual_materialized.RowCount(), label + ": row counts differ");
	Require(expected_materialized.ColumnCount() == actual_materialized.ColumnCount(),
	        label + ": column counts differ");

	const auto expected_rows = MaterializeOrderedRows(expected_materialized);
	const auto actual_rows = MaterializeOrderedRows(actual_materialized);
	if (expected_rows != actual_rows) {
		duckdb::idx_t mismatch_index = 0;
		while (mismatch_index < expected_rows.size() && mismatch_index < actual_rows.size() &&
		       expected_rows[mismatch_index] == actual_rows[mismatch_index]) {
			mismatch_index++;
		}
		throw std::runtime_error(
		    label + ": ordered rows differ at index " + std::to_string(mismatch_index) + ", expected=" +
		    (mismatch_index < expected_rows.size() ? expected_rows[mismatch_index] : std::string("<missing>")) +
		    ", actual=" +
		    (mismatch_index < actual_rows.size() ? actual_rows[mismatch_index] : std::string("<missing>")));
	}
}

void RequireOrderedByNationThenYearDesc(duckdb::MaterializedQueryResult &result, const std::string &label) {
	for (duckdb::idx_t row = 1; row < result.RowCount(); row++) {
		const auto prev_nation = result.GetValue(0, row - 1).ToString();
		const auto cur_nation = result.GetValue(0, row).ToString();
		if (prev_nation != cur_nation) {
			Require(prev_nation < cur_nation, label + ": nation not ascending at row " + std::to_string(row));
			continue;
		}
		const auto prev_year = result.GetValue(1, row - 1).GetValue<int64_t>();
		const auto cur_year = result.GetValue(1, row).GetValue<int64_t>();
		Require(prev_year > cur_year, label + ": o_year not descending within nation at row " + std::to_string(row));
	}
}

void RequireQueryErrorContains(duckdb::Connection &connection, const std::string &label, const std::string &sql,
                               const std::string &needle) {
	auto result = connection.Query(sql);
	Require(result != nullptr, label + ": query returned null result");
	Require(result->HasError(), label + ": expected query to fail");
	Require(result->GetError().find(needle) != std::string::npos,
	        label + ": expected error to contain '" + needle + "' but saw: " + result->GetError());
}

void RequireExpectedQ9Schema(duckdb::Connection &connection) {
	auto result = RunQuery(connection, AcceleratorQ9Sql("powder"));
	const duckdb::vector<duckdb::string> expected_names = {"nation", "o_year", "sum_profit"};
	const duckdb::vector<duckdb::LogicalType> expected_types = {
	    duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::BIGINT,
	    duckdb::LogicalType::DECIMAL(38, 4),
	};
	Require(result->names == expected_names, "q9 schema names differ");
	Require(result->types == expected_types, "q9 schema types differ");
}

void CreateQ9Schema(duckdb::Connection &connection, const std::string &nationkey_type = "INTEGER") {
	auto result = connection.Query(
	    "CREATE TABLE part("
	    "p_partkey BIGINT NOT NULL, "
	    "p_name VARCHAR);"
	    "CREATE TABLE supplier("
	    "s_suppkey BIGINT NOT NULL, "
	    "s_nationkey " +
	    nationkey_type +
	    " NOT NULL);"
	    "CREATE TABLE nation("
	    "n_nationkey " +
	    nationkey_type +
	    " NOT NULL, "
	    "n_name VARCHAR NOT NULL);"
	    "CREATE TABLE orders("
	    "o_orderkey BIGINT NOT NULL, "
	    "o_orderdate DATE NOT NULL);"
	    "CREATE TABLE partsupp("
	    "ps_partkey BIGINT NOT NULL, "
	    "ps_suppkey BIGINT NOT NULL, "
	    "ps_supplycost DECIMAL(15,2) NOT NULL);"
	    "CREATE TABLE lineitem("
	    "l_orderkey BIGINT NOT NULL, "
	    "l_partkey BIGINT NOT NULL, "
	    "l_suppkey BIGINT NOT NULL, "
	    "l_quantity DECIMAL(15,2) NOT NULL, "
	    "l_extendedprice DECIMAL(15,2) NOT NULL, "
	    "l_discount DECIMAL(15,2) NOT NULL);");
	Require(result != nullptr, "failed to create q9 schema");
	Require(!result->HasError(), result->GetError());
}

void SetupSyntheticFixture(duckdb::Connection &connection) {
	CreateQ9Schema(connection);
	auto result = connection.Query(R"SQL(
		INSERT INTO nation VALUES
			(0, 'ALGERIA'),
			(1, 'ARGENTINA'),
			(2, 'BRAZIL');

		INSERT INTO supplier VALUES
			(10, 0),
			(11, 1),
			(12, 2);

		INSERT INTO part VALUES
			(100, 'green powder metal'),     -- color at end-ish / middle
			(101, 'powder red paint'),       -- color at beginning
			(102, 'shiny metal powderbox'),  -- color at end substring
			(103, 'blue steel'),             -- no match
			(104, 'POWDER uppercase'),       -- case sensitivity: no match for 'powder'
			(105, NULL),                    -- null name
			(106, 'mid powder mid'),         -- color in middle
			(107, 'equal profit part');

		INSERT INTO orders VALUES
			(1000, DATE '1995-03-15'),
			(1001, DATE '1996-07-01'),
			(1002, DATE '1994-01-01'),
			(1003, DATE '1995-12-31');

		INSERT INTO partsupp VALUES
			(100, 10, 10.00),
			(101, 11, 5.00),
			(102, 12, 20.00),
			(104, 10, 1.00),
			(106, 10, 1.00),
			(106, 11, 2.00),
			(107, 10, 50.00),
			(107, 11, 50.00),
			-- mismatched pair intentionally absent for (100, 11)
			(103, 10, 1.00);

		INSERT INTO lineitem VALUES
			-- part 100 / supplier 10 / order 1000: revenue 100*(1-0.1)=90, cost 10*2=20 => 70
			(1000, 100, 10, 2.00, 100.00, 0.10),
			-- duplicate contribution same group
			(1000, 100, 10, 1.00, 50.00, 0.00),
			-- part 101 beginning match, nation ARGENTINA, year 1996
			(1001, 101, 11, 1.00, 40.00, 0.00),
			-- part 102 end match, nation BRAZIL, year 1994
			(1002, 102, 12, 2.00, 30.00, 0.00),
			-- non-matching part name
			(1000, 103, 10, 1.00, 999.00, 0.00),
			-- uppercase POWDER must not match color 'powder'
			(1000, 104, 10, 1.00, 999.00, 0.00),
			-- null p_name part
			(1000, 105, 10, 1.00, 999.00, 0.00),
			-- middle color, multiple suppliers/years
			(1000, 106, 10, 1.00, 20.00, 0.00),
			(1003, 106, 11, 1.00, 10.00, 0.00),
			-- missing partsupp pair (100, 11)
			(1000, 100, 11, 1.00, 80.00, 0.00),
			-- negative profit: revenue 10, cost 50*1=50 => -40
			(1001, 107, 10, 1.00, 10.00, 0.00),
			-- equal grouped profit across nations for part 107
			(1001, 107, 11, 1.00, 10.00, 0.00),
			-- repeated identical lineitem rows
			(1002, 102, 12, 1.00, 5.00, 0.00),
			(1002, 102, 12, 1.00, 5.00, 0.00);
	)SQL");
	Require(result != nullptr, "failed to populate synthetic fixture");
	Require(!result->HasError(), result->GetError());
}

void SetupEmptyResultFixture(duckdb::Connection &connection) {
	CreateQ9Schema(connection);
	auto result = connection.Query(R"SQL(
		INSERT INTO nation VALUES (0, 'ALGERIA');
		INSERT INTO supplier VALUES (10, 0);
		INSERT INTO part VALUES (100, 'blue steel');
		INSERT INTO orders VALUES (1000, DATE '1995-03-15');
		INSERT INTO partsupp VALUES (100, 10, 1.00);
		INSERT INTO lineitem VALUES (1000, 100, 10, 1.00, 10.00, 0.00);
	)SQL");
	Require(result != nullptr && !result->HasError(), "failed to populate empty-result fixture");
}

void SetupTransactionFixture(duckdb::Connection &connection) {
	CreateQ9Schema(connection);
	auto result = connection.Query(R"SQL(
		INSERT INTO nation VALUES
			(0, 'ALGERIA'),
			(1, 'ARGENTINA');
		INSERT INTO supplier VALUES
			(10, 0),
			(11, 1);
		INSERT INTO part VALUES
			(100, 'green powder metal'),
			(101, 'blue steel');
		INSERT INTO orders VALUES
			(1000, DATE '1995-03-15'),
			(1001, DATE '1996-07-01');
		INSERT INTO partsupp VALUES
			(100, 10, 10.00),
			(101, 11, 5.00);
		INSERT INTO lineitem VALUES
			(1000, 100, 10, 1.00, 100.00, 0.00),
			(1001, 101, 11, 1.00, 50.00, 0.00);
	)SQL");
	Require(result != nullptr && !result->HasError(), "failed to populate transaction fixture");
}

} // namespace

int main() {
	try {
		{
			duckdb::DuckDB file_database(GetTpchDbPath().c_str());
			duckdb::Connection connection(file_database);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			auto pragma = connection.Query("PRAGMA threads=1");
			Require(pragma != nullptr && !pragma->HasError(), "failed to set PRAGMA threads=1");

			RequireExpectedQ9Schema(connection);
			for (const auto &entry : accelerator_codegen::test_support::kQ9Manifest) {
				RequireEqualQ9Results(connection, entry.label, ReferenceQ9Sql(entry.color),
				                      AcceleratorQ9Sql(entry.color));
				auto ordered = RunQuery(connection, AcceleratorQ9Sql(entry.color));
				RequireOrderedByNationThenYearDesc(ordered->Cast<duckdb::MaterializedQueryResult>(),
				                                   std::string(entry.label) + "_order_check");

				auto seed_result = RunQuery(connection, StripTrailingSemicolon(ReadFile(entry.sql_path)));
				auto accel_result = RunQuery(connection, AcceleratorQ9Sql(entry.color));
				auto &seed_mat = seed_result->Cast<duckdb::MaterializedQueryResult>();
				auto &accel_mat = accel_result->Cast<duckdb::MaterializedQueryResult>();
				Require(seed_mat.RowCount() == accel_mat.RowCount(),
				        std::string(entry.label) + ": seed file row count differs from accelerator");
				Require(MaterializeOrderedRows(seed_mat) == MaterializeOrderedRows(accel_mat),
				        std::string(entry.label) + ": seed file rows differ from accelerator");
			}
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupSyntheticFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);

			RequireEqualQ9Results(connection, "synthetic_powder", ReferenceQ9Sql("powder"),
			                      AcceleratorQ9Sql("powder"));
			auto powder = RunQuery(connection, AcceleratorQ9Sql("powder"));
			RequireOrderedByNationThenYearDesc(powder->Cast<duckdb::MaterializedQueryResult>(), "synthetic_powder");

			RequireEqualQ9Results(connection, "no_matching_parts", ReferenceQ9Sql("nomatchcolor"),
			                      AcceleratorQ9Sql("nomatchcolor"));
			Require(RunQuery(connection, AcceleratorQ9Sql("nomatchcolor"))
			            ->Cast<duckdb::MaterializedQueryResult>()
			            .RowCount() == 0,
			        "no_matching_parts: expected empty");

			RequireEqualQ9Results(connection, "case_sensitive_uppercase", ReferenceQ9Sql("POWDER"),
			                      AcceleratorQ9Sql("POWDER"));
			auto powder_lower = RunQuery(connection, AcceleratorQ9Sql("powder"));
			auto powder_upper = RunQuery(connection, AcceleratorQ9Sql("POWDER"));
			Require(powder_lower->Cast<duckdb::MaterializedQueryResult>().RowCount() !=
			            powder_upper->Cast<duckdb::MaterializedQueryResult>().RowCount(),
			        "case_sensitive: lower/upper color should not produce identical row counts");
			RequireEqualQ9Results(connection, "case_sensitive_lowercase_vs_ref", ReferenceQ9Sql("powder"),
			                      AcceleratorQ9Sql("powder"));

			RequireEqualQ9Results(connection, "color_beginning", ReferenceQ9Sql("powder red"),
			                      AcceleratorQ9Sql("powder red"));
			RequireEqualQ9Results(connection, "color_middle", ReferenceQ9Sql("mid powder"),
			                      AcceleratorQ9Sql("mid powder"));
			RequireEqualQ9Results(connection, "color_end", ReferenceQ9Sql("powderbox"), AcceleratorQ9Sql("powderbox"));
			RequireEqualQ9Results(connection, "equal_profit_part", ReferenceQ9Sql("equal profit"),
			                      AcceleratorQ9Sql("equal profit"));
			RequireEqualQ9Results(connection, "negative_profit_part", ReferenceQ9Sql("equal profit"),
			                      AcceleratorQ9Sql("equal profit"));
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupEmptyResultFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			RequireEqualQ9Results(connection, "empty_result", ReferenceQ9Sql("powder"), AcceleratorQ9Sql("powder"));
			Require(RunQuery(connection, AcceleratorQ9Sql("powder"))->Cast<duckdb::MaterializedQueryResult>().RowCount() ==
			            0,
			        "empty_result: expected 0 rows");
		}

		{
			duckdb::DuckDB memory_database(nullptr);
			duckdb::Connection connection(memory_database);
			SetupTransactionFixture(connection);
			accelerator_codegen::RegisterAcceleratorFunctions(connection);

			RequireEqualQ9Results(connection, "transaction_baseline", ReferenceQ9Sql("powder"),
			                      AcceleratorQ9Sql("powder"));

			connection.BeginTransaction();

			auto part_enter = connection.Query("UPDATE part SET p_name = 'blue powder steel' WHERE p_partkey = 101");
			Require(part_enter != nullptr && !part_enter->HasError(), "part enter color failed");
			RequireEqualQ9Results(connection, "after_part_enter_color", ReferenceQ9Sql("powder"),
			                      AcceleratorQ9Sql("powder"));

			auto part_leave = connection.Query("UPDATE part SET p_name = 'blue steel' WHERE p_partkey = 101");
			Require(part_leave != nullptr && !part_leave->HasError(), "part leave color failed");
			RequireEqualQ9Results(connection, "after_part_leave_color", ReferenceQ9Sql("powder"),
			                      AcceleratorQ9Sql("powder"));

			auto supplier_nation = connection.Query("UPDATE supplier SET s_nationkey = 1 WHERE s_suppkey = 10");
			Require(supplier_nation != nullptr && !supplier_nation->HasError(), "supplier nation change failed");
			RequireEqualQ9Results(connection, "after_supplier_nation_change", ReferenceQ9Sql("powder"),
			                      AcceleratorQ9Sql("powder"));

			auto nation_name = connection.Query("UPDATE nation SET n_name = 'ALGERIA_X' WHERE n_nationkey = 0");
			Require(nation_name != nullptr && !nation_name->HasError(), "nation name update failed");
			RequireEqualQ9Results(connection, "after_nation_name_update", ReferenceQ9Sql("powder"),
			                      AcceleratorQ9Sql("powder"));

			auto order_year = connection.Query("UPDATE orders SET o_orderdate = DATE '1997-01-01' WHERE o_orderkey = 1000");
			Require(order_year != nullptr && !order_year->HasError(), "order year change failed");
			RequireEqualQ9Results(connection, "after_order_year_change", ReferenceQ9Sql("powder"),
			                      AcceleratorQ9Sql("powder"));

			auto supplycost = connection.Query("UPDATE partsupp SET ps_supplycost = 25.00 WHERE ps_partkey = 100");
			Require(supplycost != nullptr && !supplycost->HasError(), "partsupp supplycost update failed");
			RequireEqualQ9Results(connection, "after_supplycost_update", ReferenceQ9Sql("powder"),
			                      AcceleratorQ9Sql("powder"));

			auto lineitem_update = connection.Query(
			    "UPDATE lineitem SET l_partkey = 100, l_suppkey = 10, l_orderkey = 1000, "
			    "l_quantity = 2.00, l_extendedprice = 120.00, l_discount = 0.05 "
			    "WHERE l_orderkey = 1000");
			Require(lineitem_update != nullptr && !lineitem_update->HasError(), "lineitem update failed");
			RequireEqualQ9Results(connection, "after_lineitem_update", ReferenceQ9Sql("powder"),
			                      AcceleratorQ9Sql("powder"));

			auto insert_nation = connection.Query("INSERT INTO nation VALUES (3, 'CANADA')");
			Require(insert_nation != nullptr && !insert_nation->HasError(), "nation insert failed");
			auto insert_supplier = connection.Query("INSERT INTO supplier VALUES (12, 3)");
			Require(insert_supplier != nullptr && !insert_supplier->HasError(), "supplier insert failed");
			auto insert_part = connection.Query("INSERT INTO part VALUES (200, 'fresh powder batch')");
			Require(insert_part != nullptr && !insert_part->HasError(), "part insert failed");
			auto insert_orders = connection.Query("INSERT INTO orders VALUES (2000, DATE '1998-05-05')");
			Require(insert_orders != nullptr && !insert_orders->HasError(), "orders insert failed");
			auto insert_partsupp = connection.Query("INSERT INTO partsupp VALUES (200, 12, 3.00)");
			Require(insert_partsupp != nullptr && !insert_partsupp->HasError(), "partsupp insert failed");
			auto insert_lineitem =
			    connection.Query("INSERT INTO lineitem VALUES (2000, 200, 12, 1.00, 30.00, 0.00)");
			Require(insert_lineitem != nullptr && !insert_lineitem->HasError(), "lineitem insert failed");
			RequireEqualQ9Results(connection, "after_inserts", ReferenceQ9Sql("powder"), AcceleratorQ9Sql("powder"));

			auto delete_lineitem = connection.Query("DELETE FROM lineitem WHERE l_orderkey = 2000");
			Require(delete_lineitem != nullptr && !delete_lineitem->HasError(), "lineitem delete failed");
			auto delete_partsupp = connection.Query("DELETE FROM partsupp WHERE ps_partkey = 200");
			Require(delete_partsupp != nullptr && !delete_partsupp->HasError(), "partsupp delete failed");
			auto delete_orders = connection.Query("DELETE FROM orders WHERE o_orderkey = 2000");
			Require(delete_orders != nullptr && !delete_orders->HasError(), "orders delete failed");
			auto delete_part = connection.Query("DELETE FROM part WHERE p_partkey = 200");
			Require(delete_part != nullptr && !delete_part->HasError(), "part delete failed");
			auto delete_supplier = connection.Query("DELETE FROM supplier WHERE s_suppkey = 12");
			Require(delete_supplier != nullptr && !delete_supplier->HasError(), "supplier delete failed");
			auto delete_nation = connection.Query("DELETE FROM nation WHERE n_nationkey = 3");
			Require(delete_nation != nullptr && !delete_nation->HasError(), "nation delete failed");
			RequireEqualQ9Results(connection, "after_deletes", ReferenceQ9Sql("powder"), AcceleratorQ9Sql("powder"));

			connection.Rollback();
			RequireEqualQ9Results(connection, "after_rollback", ReferenceQ9Sql("powder"), AcceleratorQ9Sql("powder"));

			connection.BeginTransaction();
			auto commit_part = connection.Query("UPDATE part SET p_name = 'committed powder' WHERE p_partkey = 101");
			Require(commit_part != nullptr && !commit_part->HasError(), "committed part update failed");
			connection.Commit();
			RequireEqualQ9Results(connection, "after_commit_visible", ReferenceQ9Sql("powder"),
			                      AcceleratorQ9Sql("powder"));
		}

		{
			duckdb::DuckDB mismatch_database(nullptr);
			duckdb::Connection connection(mismatch_database);
			CreateQ9Schema(connection, "BIGINT");
			accelerator_codegen::RegisterAcceleratorFunctions(connection);
			RequireQueryErrorContains(connection, "schema_mismatch", AcceleratorQ9Sql("powder"), "Unsupported schema");
		}

		std::cout << "tpch_q9_smoke_test passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &ex) {
		std::cerr << "tpch_q9_smoke_test failed: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
