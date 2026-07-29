#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q10_aux.hpp"
#include "test_support/q10_aux_helpers.hpp"

#include <iostream>
#include <stdexcept>

#include "duckdb.hpp"

int main() {
	try {
		duckdb::DuckDB db(nullptr);
		duckdb::Connection c(db);
		c.Query("CREATE TABLE nation(n_nationkey INTEGER NOT NULL, n_name VARCHAR NOT NULL, n_regionkey INTEGER NOT NULL, "
		        "n_comment VARCHAR NOT NULL)");
		c.Query("CREATE TABLE customer(c_custkey BIGINT NOT NULL, c_name VARCHAR NOT NULL, c_address VARCHAR NOT NULL, "
		        "c_nationkey INTEGER NOT NULL, c_phone VARCHAR NOT NULL, c_acctbal DECIMAL(15,2) NOT NULL, "
		        "c_mktsegment VARCHAR NOT NULL, c_comment VARCHAR NOT NULL)");
		c.Query("CREATE TABLE orders(o_orderkey BIGINT NOT NULL, o_custkey BIGINT NOT NULL, o_orderstatus VARCHAR NOT NULL, "
		        "o_totalprice DECIMAL(15,2) NOT NULL, o_orderdate DATE NOT NULL, o_orderpriority VARCHAR NOT NULL, "
		        "o_clerk VARCHAR NOT NULL, o_shippriority INTEGER NOT NULL, o_comment VARCHAR NOT NULL)");
		c.Query("CREATE TABLE lineitem(l_orderkey BIGINT NOT NULL, l_partkey BIGINT NOT NULL, l_suppkey BIGINT NOT NULL, "
		        "l_linenumber BIGINT NOT NULL, l_quantity DECIMAL(15,2) NOT NULL, l_extendedprice DECIMAL(15,2) NOT NULL, "
		        "l_discount DECIMAL(15,2) NOT NULL, l_tax DECIMAL(15,2) NOT NULL, l_returnflag VARCHAR NOT NULL, "
		        "l_linestatus VARCHAR NOT NULL, l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL, "
		        "l_receiptdate DATE NOT NULL, l_shipinstruct VARCHAR NOT NULL, l_shipmode VARCHAR NOT NULL, "
		        "l_comment VARCHAR NOT NULL)");
		c.Query("INSERT INTO nation VALUES (0, 'NATION0', 0, 'n')");
		c.Query("INSERT INTO customer VALUES (1, 'C1', 'ADDR', 0, 'PHONE', 1000.00, 'BUILDING', 'c')");
		c.Query("INSERT INTO orders VALUES (100, 1, 'O', 1000.00, DATE '1993-06-15', '1-URGENT', 'c', 0, 'c')");
		c.Query("INSERT INTO lineitem VALUES (100, 1, 1, 1, 1.00, 100.00, 0.00, 0.00, 'R', 'O', "
		        "DATE '1993-06-10', DATE '1993-06-15', DATE '1993-06-20', 'DELIVER IN PERSON', 'MAIL', 'c')");
		accelerator_codegen::RegisterAcceleratorFunctions(c);
		c.Query("SELECT * FROM accel_q10_aux_build()");
		accelerator_codegen::test_support::RequireAuxMonthPartitionReadable(c, "baseline", duckdb::Date::FromString("1993-06-15"));
		accelerator_codegen::test_support::RequireEqualQ10Results(
		    c, "query", accelerator_codegen::test_support::ReferenceQ10Sql("1993-06-01"),
		    accelerator_codegen::test_support::AcceleratorQ10AuxSql("1993-06-01"));
		std::cout << "test_q10_aux_partition_state passed\n";
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << ex.what() << std::endl;
		return 1;
	}
}
