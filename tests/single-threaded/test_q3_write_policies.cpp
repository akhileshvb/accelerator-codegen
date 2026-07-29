#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q3_write_policy.hpp"
#include "test_support/q3_aux_helpers.hpp"

#include <cstdlib>
#include <iostream>

#include "duckdb.hpp"

int main() {
	try {
		accelerator_codegen::ResetQ3AuxCounters();
		setenv("Q3_FORCE_ACTION", "InvalidateAndFallback", 1);
		duckdb::DuckDB db(nullptr);
		duckdb::Connection c(db);
		c.Query("CREATE TABLE customer(c_custkey BIGINT, c_name VARCHAR, c_address VARCHAR, c_nationkey BIGINT, c_phone VARCHAR, c_acctbal DECIMAL(15,2), c_mktsegment VARCHAR, c_comment VARCHAR)");
		c.Query("CREATE TABLE orders(o_orderkey BIGINT, o_custkey BIGINT, o_orderstatus VARCHAR, o_totalprice DECIMAL(15,2), o_orderdate DATE, o_orderpriority VARCHAR, o_clerk VARCHAR, o_shippriority INTEGER, o_comment VARCHAR)");
		c.Query("CREATE TABLE lineitem(l_orderkey BIGINT, l_partkey BIGINT, l_suppkey BIGINT, l_linenumber BIGINT, l_quantity DECIMAL(15,2), l_extendedprice DECIMAL(15,2), l_discount DECIMAL(15,2), l_tax DECIMAL(15,2), l_returnflag VARCHAR, l_linestatus VARCHAR, l_shipdate DATE, l_commitdate DATE, l_receiptdate DATE, l_shipinstruct VARCHAR, l_shipmode VARCHAR, l_comment VARCHAR)");
		c.Query("INSERT INTO customer VALUES (1,'n','a',1,'p',1,'BUILDING','c')");
		c.Query("INSERT INTO orders VALUES (1,1,'O',1,DATE '1995-03-01','5-LOW','c',0,'c')");
		c.Query("INSERT INTO lineitem VALUES (1,1,1,1,1,100,0,0,'N','O',DATE '1995-03-20',DATE '1995-03-20',DATE '1995-03-20','NONE','AIR','c')");
		accelerator_codegen::RegisterAcceleratorFunctions(c);
		c.Query("SELECT * FROM accel_q3_aux_build()");
		accelerator_codegen::test_support::RequireEqualQ3Results(c, "forced_fallback", accelerator_codegen::test_support::ReferenceQ3Sql("BUILDING", "1995-03-15"), accelerator_codegen::test_support::AcceleratorQ3AuxSql("BUILDING", "1995-03-15"));
		unsetenv("Q3_FORCE_ACTION");
		accelerator_codegen::test_support::ClearQ3MaintenanceOverrides();
		std::cout << "test_q3_write_policies passed\n";
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << ex.what() << std::endl;
		return 1;
	}
}
