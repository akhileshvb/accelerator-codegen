#include "accelerator_codegen/register.hpp"
#include "test_support/q6_aux_helpers.hpp"

#include <iostream>
#include <stdexcept>

#include "duckdb.hpp"

int main() {
	try {
		duckdb::DuckDB db(nullptr);
		duckdb::Connection c(db);
		c.Query("CREATE TABLE lineitem(l_orderkey BIGINT NOT NULL, l_partkey BIGINT NOT NULL, l_suppkey BIGINT NOT NULL, "
		        "l_linenumber BIGINT NOT NULL, l_quantity DECIMAL(15,2) NOT NULL, l_extendedprice DECIMAL(15,2) NOT NULL, "
		        "l_discount DECIMAL(15,2) NOT NULL, l_tax DECIMAL(15,2) NOT NULL, l_returnflag VARCHAR NOT NULL, "
		        "l_linestatus VARCHAR NOT NULL, l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL, "
		        "l_receiptdate DATE NOT NULL, l_shipinstruct VARCHAR NOT NULL, l_shipmode VARCHAR NOT NULL, "
		        "l_comment VARCHAR NOT NULL)");
		c.Query("INSERT INTO lineitem VALUES (1, 10, 100, 1, 10.00, 100.00, 0.06, 0.01, 'N', 'O', DATE '1992-02-29', DATE '1992-03-01', DATE '1992-03-02', 'DELIVER', 'MAIL', 'ok')");
		accelerator_codegen::RegisterAcceleratorFunctions(c);
		c.Query("SELECT * FROM accel_q6_aux_build()");
		accelerator_codegen::test_support::RequireAuxDayPartitionReadable(c, "baseline", duckdb::Date::FromString("1992-02-29"));
		accelerator_codegen::test_support::RequireEqualQ6Revenue(
		    c, "query", accelerator_codegen::test_support::ReferenceQ6Sql("1992-01-01", "0.06", "24.00"),
		    accelerator_codegen::test_support::AcceleratorQ6AuxSql("1992-01-01", "0.06", "24.00"));
		std::cout << "test_q6_aux_partition_state passed\n";
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << ex.what() << std::endl;
		return 1;
	}
}
