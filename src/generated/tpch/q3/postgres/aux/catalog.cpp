// GENERATED BACKEND — PostgreSQL catalog entry for TPC-H Q3.
#include "catalog/postgres/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

const PostgresQueryCatalogRegistrar kRegistrar(PostgresSharedQueryCatalogEntry
    {
        3,
        "tpch",
        "tpch_q3",
        /* base_sql */
        "SELECT l_orderkey, revenue, o_orderdate, o_shippriority FROM ("
        "SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, "
        "o_orderdate, o_shippriority "
        "FROM customer, orders, lineitem "
        "WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey AND "
        "l_orderkey = o_orderkey "
        "AND o_orderdate < DATE '1995-03-15' AND l_shipdate > DATE '1995-03-15' "
        "GROUP BY l_orderkey, o_orderdate, o_shippriority "
        "ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority LIMIT 10)",
        /* accelerator_sql */
        "SELECT * FROM accel_tpch_q3('BUILDING', DATE '1995-03-15')",
        /* aux_sql */
        "SELECT * FROM accel_tpch_q3_aux('BUILDING', DATE '1995-03-15')",
    }
);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres
