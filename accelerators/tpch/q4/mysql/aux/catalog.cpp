// MySQL query catalog entry for TPC-H Q4, registered at load time.
#include "backends/mysql/api/catalog/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

const MysqlQueryCatalogRegistrar kRegisterTpchQ4(MysqlSharedQueryCatalogEntry
    {
        4,
        "tpch",
        "tpch_q4",
        /* base_sql */
        "SELECT o_orderpriority, COUNT(o_orderkey) AS order_count "
        "FROM orders "
        "WHERE o_orderdate >= DATE '1993-07-01' "
        "AND o_orderdate < DATE_ADD(DATE '1993-07-01', INTERVAL 3 MONTH) "
        "AND EXISTS ("
        "SELECT * FROM lineitem WHERE l_orderkey = o_orderkey "
        "AND l_commitdate < l_receiptdate) "
        "GROUP BY o_orderpriority ORDER BY o_orderpriority",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q4_aux(DATE '1993-07-01')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q4_aux(DATE '1993-07-01')",
        /* fallback_sql */
        "SELECT o_orderpriority, COUNT(o_orderkey) AS order_count "
        "FROM orders WHERE o_orderdate >= $1 AND o_orderdate < DATE_ADD($1, INTERVAL 3 MONTH) "
        "AND EXISTS (SELECT * FROM lineitem WHERE l_orderkey = o_orderkey AND l_commitdate < l_receiptdate) "
        "GROUP BY o_orderpriority ORDER BY o_orderpriority"
    });

} // namespace
} // namespace accelerator_codegen::aux_state::engine::mysql
