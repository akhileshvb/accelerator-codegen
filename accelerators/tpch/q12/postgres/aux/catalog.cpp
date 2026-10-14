// GENERATED BACKEND — PostgreSQL catalog entry for TPC-H Q12.
#include "backends/postgres/api/catalog/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

const PostgresQueryCatalogRegistrar kRegistrar(PostgresSharedQueryCatalogEntry
    {
        12,
        "tpch",
        "tpch_q12",
        /* base_sql */
        "SELECT l_shipmode, "
        "SUM(CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH' "
        "THEN 1 ELSE 0 END) AS high_line_count, "
        "SUM(CASE WHEN o_orderpriority <> '1-URGENT' AND o_orderpriority <> "
        "'2-HIGH' THEN 1 ELSE 0 END) AS low_line_count "
        "FROM orders, lineitem "
        "WHERE o_orderkey = l_orderkey AND l_shipmode IN ('MAIL', 'SHIP') "
        "AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate "
        "AND l_receiptdate >= DATE '1995-01-01' "
        "AND l_receiptdate < DATE '1995-01-01' + INTERVAL '1' year "
        "GROUP BY l_shipmode ORDER BY l_shipmode",
        /* accelerator_sql */
        "SELECT * FROM accel_tpch_q12('MAIL', 'SHIP', DATE '1995-01-01')",
        /* aux_sql */
        "SELECT * FROM accel_tpch_q12_aux('MAIL', 'SHIP', DATE '1995-01-01')",
    }
);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres
