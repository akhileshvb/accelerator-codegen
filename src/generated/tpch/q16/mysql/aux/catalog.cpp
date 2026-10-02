// MySQL query catalog entry for TPC-H Q16, registered at load time.
#include "catalog/mysql/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

const MysqlQueryCatalogRegistrar kRegisterTpchQ16(MysqlSharedQueryCatalogEntry
    {
        16,
        "tpch",
        "tpch_q16",
        /* base_sql */
        "SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS supplier_cnt "
        "FROM partsupp, part "
        "WHERE p_partkey = ps_partkey AND p_brand <> 'Brand#13' "
        "AND p_type NOT LIKE 'SMALL BRUSHED%' "
        "AND p_size IN (26, 24, 27, 29, 7, 20, 22, 33) "
        "AND ps_suppkey NOT IN ("
        "SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%') "
        "GROUP BY p_brand, p_type, p_size "
        "ORDER BY supplier_cnt DESC, p_brand, p_type, p_size",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q16_aux('Brand#13', 'SMALL BRUSHED%', 26, 24, 27, 29, 7, 20, 22, 33)",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q16_aux('Brand#13', 'SMALL BRUSHED%', 26, 24, 27, 29, 7, 20, 22, 33)",
    });

} // namespace
} // namespace accelerator_codegen::aux_state::engine::mysql
