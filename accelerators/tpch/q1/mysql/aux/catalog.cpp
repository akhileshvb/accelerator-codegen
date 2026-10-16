// MySQL query catalog entry for TPC-H Q1, registered at load time.
#include "backends/mysql/api/catalog/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

const MysqlQueryCatalogRegistrar kRegisterTpchQ1(MysqlSharedQueryCatalogEntry
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
        "WHERE l_shipdate <= DATE_SUB(DATE '1998-12-01', INTERVAL 90 DAY) "
        "GROUP BY l_returnflag, l_linestatus "
        "ORDER BY l_returnflag, l_linestatus",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q1_aux(90)",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q1_aux(90)",
        /* fallback_sql */
        "SELECT l_returnflag, l_linestatus, "
        "sum(l_quantity) AS sum_qty, sum(l_extendedprice) AS sum_base_price, "
        "sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price, "
        "sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge, "
        "avg(l_quantity) AS avg_qty, avg(l_extendedprice) AS avg_price, "
        "avg(l_discount) AS avg_disc, count(*) AS count_order "
        "FROM lineitem "
        "WHERE l_shipdate <= DATE_SUB(DATE '1998-12-01', INTERVAL $1 DAY) "
        "GROUP BY l_returnflag, l_linestatus "
        "ORDER BY l_returnflag, l_linestatus"
    });

} // namespace
} // namespace accelerator_codegen::aux_state::engine::mysql
