// GENERATED BACKEND — PostgreSQL catalog entry for TPC-H Q14.
#include "backends/postgres/api/catalog/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

const PostgresQueryCatalogRegistrar kRegistrar(PostgresSharedQueryCatalogEntry
    {
        14,
        "tpch",
        "tpch_q14",
        /* base_sql */
        "SELECT 100.00 * SUM(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * "
        "(1 - l_discount) ELSE 0 END) / "
        "SUM(l_extendedprice * (1 - l_discount)) AS promo_revenue "
        "FROM lineitem, part "
        "WHERE l_partkey = p_partkey "
        "AND l_shipdate >= DATE '1995-06-01' AND l_shipdate < DATE '1995-06-01' + "
        "INTERVAL '1' month",
        /* accelerator_sql */
        "SELECT * FROM accel_tpch_q14(DATE '1995-06-01')",
        /* aux_sql */
        "SELECT * FROM accel_tpch_q14_aux(DATE '1995-06-01')",
    }
);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres
