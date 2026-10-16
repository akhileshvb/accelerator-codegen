// GENERATED BACKEND — PostgreSQL catalog entry for TPC-H Q6.
#include "backends/postgres/api/catalog/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

const PostgresQueryCatalogRegistrar kRegistrar(PostgresSharedQueryCatalogEntry
    {
        6,
        "tpch",
        "tpch_q6",
        /* base_sql */
        "SELECT SUM(l_extendedprice * l_discount) AS revenue "
        "FROM lineitem "
        "WHERE l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01' "
        "AND l_discount BETWEEN 0.02 AND 0.04 AND l_quantity < 25",
        /* accelerator_sql */
        "SELECT revenue FROM accel_tpch_q6(DATE '1995-01-01', 0.03, 25)",
        /* aux_sql */
        "SELECT revenue FROM accel_tpch_q6_aux(DATE '1995-01-01', 0.03, 25)",
        /* fallback_sql */
        "SELECT SUM(l_extendedprice * l_discount) AS revenue "
        "FROM lineitem "
        "WHERE l_shipdate >= $1 AND l_shipdate < $1 + INTERVAL '1' year "
        "AND l_discount BETWEEN $2::numeric - 0.01 AND $2::numeric + 0.01 AND l_quantity < $3::numeric"
    }
);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres
