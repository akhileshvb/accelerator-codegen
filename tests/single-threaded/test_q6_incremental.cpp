#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q6_aux.hpp"
#include "accelerator_codegen/tpch_q6_write_policy.hpp"
#include "test_support/q6_aux_helpers.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

#include "duckdb.hpp"

namespace {

void Require(bool ok, const std::string &msg) {
	if (!ok) throw std::runtime_error(msg);
}

void CreateSchema(duckdb::Connection &c) {
	Require(c.Query("CREATE TABLE lineitem(l_orderkey BIGINT NOT NULL, l_partkey BIGINT NOT NULL, l_suppkey BIGINT NOT NULL, "
	                "l_linenumber BIGINT NOT NULL, l_quantity DECIMAL(15,2) NOT NULL, l_extendedprice DECIMAL(15,2) NOT NULL, "
	                "l_discount DECIMAL(15,2) NOT NULL, l_tax DECIMAL(15,2) NOT NULL, l_returnflag VARCHAR NOT NULL, "
	                "l_linestatus VARCHAR NOT NULL, l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL, "
	                "l_receiptdate DATE NOT NULL, l_shipinstruct VARCHAR NOT NULL, l_shipmode VARCHAR NOT NULL, "
	                "l_comment VARCHAR NOT NULL)") != nullptr, "schema lineitem");
}

} // namespace

int main() {
	try {
		accelerator_codegen::test_support::EnableHybridMaintenance();
		duckdb::DuckDB db(nullptr);
		duckdb::Connection c(db);
		CreateSchema(c);
		c.Query("INSERT INTO lineitem VALUES (1, 10, 100, 1, 10.00, 100.00, 0.06, 0.01, 'N', 'O', DATE '1992-02-29', DATE '1992-03-01', DATE '1992-03-02', 'DELIVER', 'MAIL', 'ok')");
		accelerator_codegen::RegisterAcceleratorFunctions(c);
		c.Query("SELECT * FROM accel_q6_aux_build()");
		accelerator_codegen::test_support::RequireEqualQ6Revenue(
		    c, "baseline", accelerator_codegen::test_support::ReferenceQ6Sql("1992-01-01", "0.06", "24.00"),
		    accelerator_codegen::test_support::AcceleratorQ6AuxSql("1992-01-01", "0.06", "24.00"));
		c.BeginTransaction();
		c.Query("INSERT INTO lineitem VALUES (2, 20, 200, 1, 20.00, 200.00, 0.06, 0.02, 'N', 'O', DATE '1992-06-01', DATE '1992-06-02', DATE '1992-06-03', 'DELIVER', 'MAIL', 'insert')");
		c.Commit();
		accelerator_codegen::test_support::RequireEqualQ6Revenue(
		    c, "after_insert", accelerator_codegen::test_support::ReferenceQ6Sql("1992-01-01", "0.06", "24.00"),
		    accelerator_codegen::test_support::AcceleratorQ6AuxSql("1992-01-01", "0.06", "24.00"));
		Require(accelerator_codegen::GetQ6AuxDiagnostics().used_aux_path, "aux path");
		accelerator_codegen::test_support::RequireNoIncrementalFailures("incremental");
		std::cout << "test_q6_incremental passed\n";
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << "test_q6_incremental failed: " << ex.what() << std::endl;
		return 1;
	}
}
