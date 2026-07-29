#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q3_aux.hpp"
#include "accelerator_codegen/tpch_q3_write_policy.hpp"
#include "test_support/q3_aux_helpers.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

#include "duckdb.hpp"

namespace {

void Require(bool ok, const std::string &msg) {
	if (!ok) throw std::runtime_error(msg);
}

void CreateSchema(duckdb::Connection &c) {
	Require(c.Query("CREATE TABLE customer(c_custkey BIGINT, c_name VARCHAR, c_address VARCHAR, c_nationkey BIGINT, "
	                "c_phone VARCHAR, c_acctbal DECIMAL(15,2), c_mktsegment VARCHAR, c_comment VARCHAR)") != nullptr,
	        "schema");
	Require(c.Query("CREATE TABLE orders(o_orderkey BIGINT, o_custkey BIGINT, o_orderstatus VARCHAR, "
	                "o_totalprice DECIMAL(15,2), o_orderdate DATE, o_orderpriority VARCHAR, o_clerk VARCHAR, "
	                "o_shippriority INTEGER, o_comment VARCHAR)") != nullptr, "schema");
	Require(c.Query("CREATE TABLE lineitem(l_orderkey BIGINT, l_partkey BIGINT, l_suppkey BIGINT, l_linenumber BIGINT, "
	                "l_quantity DECIMAL(15,2), l_extendedprice DECIMAL(15,2), l_discount DECIMAL(15,2), "
	                "l_tax DECIMAL(15,2), l_returnflag VARCHAR, l_linestatus VARCHAR, l_shipdate DATE, "
	                "l_commitdate DATE, l_receiptdate DATE, l_shipinstruct VARCHAR, l_shipmode VARCHAR, "
	                "l_comment VARCHAR)") != nullptr, "schema");
}

} // namespace

int main() {
	try {
		accelerator_codegen::test_support::EnableHybridMaintenance();
		duckdb::DuckDB db(nullptr);
		duckdb::Connection c(db);
		CreateSchema(c);
		c.Query("INSERT INTO customer VALUES (1,'n','a',1,'p',1,'BUILDING','c')");
		c.Query("INSERT INTO orders VALUES (1,1,'O',1,DATE '1995-03-01','5-LOW','c',0,'c')");
		c.Query("INSERT INTO lineitem VALUES (1,1,1,1,1,100,0,0,'N','O',DATE '1995-03-20',DATE '1995-03-20',"
		        "DATE '1995-03-20','NONE','AIR','c')");
		accelerator_codegen::RegisterAcceleratorFunctions(c);
		c.Query("SELECT * FROM accel_q3_aux_build()");
		accelerator_codegen::test_support::RequireEqualQ3Results(
		    c, "baseline", accelerator_codegen::test_support::ReferenceQ3Sql("BUILDING", "1995-03-15"),
		    accelerator_codegen::test_support::AcceleratorQ3AuxSql("BUILDING", "1995-03-15"));
		c.BeginTransaction();
		c.Query("INSERT INTO lineitem VALUES (1,1,1,2,1,50,0,0,'N','O',DATE '1995-03-25',DATE '1995-03-25',"
		        "DATE '1995-03-25','NONE','AIR','c')");
		c.Commit();
		accelerator_codegen::test_support::RequireEqualQ3Results(
		    c, "after_insert", accelerator_codegen::test_support::ReferenceQ3Sql("BUILDING", "1995-03-15"),
		    accelerator_codegen::test_support::AcceleratorQ3AuxSql("BUILDING", "1995-03-15"));
		Require(accelerator_codegen::GetQ3AuxDiagnostics().used_aux_path, "aux path");
		std::cout << "test_q3_incremental passed\n";
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << "test_q3_incremental failed: " << ex.what() << std::endl;
		return 1;
	}
}
