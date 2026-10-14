// MySQL query catalog entry for TPC-H Q17, registered at load time.
#include "backends/mysql/api/catalog/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

const MysqlQueryCatalogRegistrar kRegisterTpchQ17(MysqlSharedQueryCatalogEntry
    {
        17,
        "tpch",
        "tpch_q17",
        /* base_sql */
        "SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly "
        "FROM lineitem, part, "
        "(SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity "
        "FROM lineitem GROUP BY l_partkey) part_agg "
        "WHERE p_partkey = l_partkey AND agg_partkey = l_partkey "
        "AND p_brand = 'Brand#34' AND p_container = 'LG CASE' "
        "AND l_quantity < avg_quantity",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q17_aux('Brand#34', 'LG CASE')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q17_aux('Brand#34', 'LG CASE')",
    });

} // namespace
} // namespace accelerator_codegen::aux_state::engine::mysql
