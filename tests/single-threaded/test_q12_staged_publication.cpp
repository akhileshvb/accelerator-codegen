#include "accelerator_codegen/register.hpp"
#include "test_support/q12_aux_helpers.hpp"

#include <iostream>

#include "duckdb.hpp"

int main() {
	try {
		duckdb::DuckDB db(nullptr);
		duckdb::Connection c(db);
		c.Query("CREATE TABLE orders(o_orderkey BIGINT NOT NULL, o_custkey BIGINT NOT NULL, o_orderstatus VARCHAR NOT NULL, "
		        "o_totalprice DECIMAL(15,2) NOT NULL, o_orderdate DATE NOT NULL, o_orderpriority VARCHAR NOT NULL, "
		        "o_clerk VARCHAR NOT NULL, o_shippriority INTEGER NOT NULL, o_comment VARCHAR NOT NULL)");
		c.Query("CREATE TABLE lineitem(l_orderkey BIGINT NOT NULL, l_partkey BIGINT NOT NULL, l_suppkey BIGINT NOT NULL, "
		        "l_linenumber BIGINT NOT NULL, l_quantity DECIMAL(15,2) NOT NULL, l_extendedprice DECIMAL(15,2) NOT NULL, "
		        "l_discount DECIMAL(15,2) NOT NULL, l_tax DECIMAL(15,2) NOT NULL, l_returnflag VARCHAR NOT NULL, "
		        "l_linestatus VARCHAR NOT NULL, l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL, "
		        "l_receiptdate DATE NOT NULL, l_shipinstruct VARCHAR NOT NULL, l_shipmode VARCHAR NOT NULL, "
		        "l_comment VARCHAR NOT NULL)");
		c.Query("INSERT INTO orders VALUES (100, 1, 'O', 1000.00, DATE '1995-03-01', '1-URGENT', 'c', 0, 'c')");
		c.Query("INSERT INTO lineitem VALUES (100, 1, 1, 1, 1.00, 100.00, 0.00, 0.00, 'N', 'O', "
		        "DATE '1995-03-10', DATE '1995-03-15', DATE '1995-03-20', 'DELIVER IN PERSON', 'MAIL', 'c')");
		accelerator_codegen::RegisterAcceleratorFunctions(c);
		c.Query("SELECT * FROM accel_q12_aux_build()");
		c.BeginTransaction();
		c.Query("INSERT INTO lineitem VALUES (100, 1, 1, 2, 1.00, 50.00, 0.00, 0.00, 'N', 'O', "
		        "DATE '1995-03-10', DATE '1995-03-15', DATE '1995-03-25', 'DELIVER IN PERSON', 'MAIL', 'c')");
		c.Commit();
		accelerator_codegen::test_support::RequireEqualQ12Results(
		    c, "staged", accelerator_codegen::test_support::ReferenceQ12Sql("MAIL", "SHIP", "1995-01-01"),
		    accelerator_codegen::test_support::AcceleratorQ12AuxSql("MAIL", "SHIP", "1995-01-01"));
		std::cout << "test_q12_staged_publication passed\n";
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << ex.what() << std::endl;
		return 1;
	}
}
