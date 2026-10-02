// MySQL query catalog entry for TPC-H Q13, registered at load time.
#include "catalog/mysql/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

const MysqlQueryCatalogRegistrar kRegisterTpchQ13(MysqlSharedQueryCatalogEntry
    {
        13,
        "tpch",
        "tpch_q13",
        /* base_sql */
        "SELECT c_count, COUNT(*) AS custdist FROM ("
        "SELECT c_custkey, COUNT(o_orderkey) AS c_count FROM customer "
        "LEFT OUTER JOIN orders ON c_custkey = o_custkey AND o_comment NOT LIKE "
        "'%unusual%accounts%' GROUP BY c_custkey) AS c_orders "
        "GROUP BY c_count ORDER BY custdist DESC, c_count DESC",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q13_aux('%unusual%accounts%')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q13_aux('%unusual%accounts%')",
    });

} // namespace
} // namespace accelerator_codegen::aux_state::engine::mysql
