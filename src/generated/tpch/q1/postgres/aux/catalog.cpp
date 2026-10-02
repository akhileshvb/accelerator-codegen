// GENERATED BACKEND — PostgreSQL catalog entry for TPC-H Q1.
#include "catalog/postgres/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

const PostgresQueryCatalogRegistrar kRegistrar(PostgresSharedQueryCatalogEntry
    {
        1,
        "tpch",
        "tpch_q1",
        /* base_sql */
        "SELECT l_returnflag, l_linestatus, "
        "sum(l_quantity) AS sum_qty, sum(l_extendedprice) AS sum_base_price, "
        "sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price, "
        "sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge, "
        "avg(l_quantity) AS avg_qty, avg(l_extendedprice) AS avg_price, "
        "avg(l_discount) AS avg_disc, count(*) AS count_order "
        "FROM lineitem "
        "WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY "
        "GROUP BY l_returnflag, l_linestatus "
        "ORDER BY l_returnflag, l_linestatus",
        /* accelerator_sql */
        "SELECT * FROM accel_tpch_q1(90)",
        /* aux_sql */
        "SELECT * FROM accel_tpch_q1_aux(90)",
    }
);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres
