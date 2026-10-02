// MySQL query catalog entry for TPC-H Q11, registered at load time.
#include "catalog/mysql/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

const MysqlQueryCatalogRegistrar kRegisterTpchQ11(MysqlSharedQueryCatalogEntry
    {
        11,
        "tpch",
        "tpch_q11",
        /* base_sql */
        "SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS value "
        "FROM partsupp, supplier, nation "
        "WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey "
        "AND n_name = 'ROMANIA' "
        "GROUP BY ps_partkey "
        "HAVING SUM(ps_supplycost * ps_availqty) > ("
        "SELECT SUM(ps_supplycost * ps_availqty) * 0.0001 "
        "FROM partsupp, supplier, nation "
        "WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey "
        "AND n_name = 'ROMANIA') "
        "ORDER BY value DESC",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q11_aux('ROMANIA', 0.0001)",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q11_aux('ROMANIA', 0.0001)",
    });

} // namespace
} // namespace accelerator_codegen::aux_state::engine::mysql
