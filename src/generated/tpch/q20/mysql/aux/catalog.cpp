// MySQL query catalog entry for TPC-H Q20, registered at load time.
#include "catalog/mysql/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

const MysqlQueryCatalogRegistrar kRegisterTpchQ20(MysqlSharedQueryCatalogEntry
    {
        20,
        "tpch",
        "tpch_q20",
        /* base_sql */
        "SELECT s_name, s_address "
        "FROM supplier, nation "
        "WHERE s_suppkey IN ("
        "SELECT ps_suppkey FROM partsupp, ("
        "SELECT l_partkey agg_partkey, l_suppkey agg_suppkey, 0.5 * SUM(l_quantity) AS agg_quantity "
        "FROM lineitem WHERE l_shipdate >= DATE '1997-01-01' "
        "AND l_shipdate < DATE_ADD(DATE '1997-01-01', INTERVAL 1 YEAR) "
        "GROUP BY l_partkey, l_suppkey) agg_lineitem "
        "WHERE agg_partkey = ps_partkey AND agg_suppkey = ps_suppkey "
        "AND ps_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE 'gainsboro%') "
        "AND ps_availqty > agg_quantity) "
        "AND s_nationkey = n_nationkey AND n_name = 'RUSSIA' "
        "ORDER BY s_name",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q20_aux(DATE '1997-01-01', 'gainsboro%', 'RUSSIA')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q20_aux(DATE '1997-01-01', 'gainsboro%', 'RUSSIA')",
    });

} // namespace
} // namespace accelerator_codegen::aux_state::engine::mysql
