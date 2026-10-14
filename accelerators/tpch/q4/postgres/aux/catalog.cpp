// GENERATED BACKEND — PostgreSQL catalog entry for TPC-H Q4.
#include "backends/postgres/api/catalog/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

const PostgresQueryCatalogRegistrar kRegistrar(PostgresSharedQueryCatalogEntry
    {
        4,
        "tpch",
        "tpch_q4",
        /* base_sql */
        "SELECT o_orderpriority, COUNT(o_orderkey) AS order_count "
        "FROM orders "
        "WHERE o_orderdate >= DATE '1993-07-01' "
        "AND o_orderdate < DATE '1993-07-01' + INTERVAL '3' month "
        "AND EXISTS ("
        "SELECT * FROM lineitem WHERE l_orderkey = o_orderkey "
        "AND l_commitdate < l_receiptdate) "
        "GROUP BY o_orderpriority ORDER BY o_orderpriority",
        /* accelerator_sql */
        "SELECT * FROM accel_tpch_q4(DATE '1993-07-01')",
        /* aux_sql */
        "SELECT * FROM accel_tpch_q4_aux(DATE '1993-07-01')",
    }
);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres
