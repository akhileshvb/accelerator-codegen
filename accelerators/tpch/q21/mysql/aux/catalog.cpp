// MySQL query catalog entry for TPC-H Q21, registered at load time.
#include "backends/mysql/api/catalog/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

const MysqlQueryCatalogRegistrar kRegisterTpchQ21(MysqlSharedQueryCatalogEntry
    {
        21,
        "tpch",
        "tpch_q21",
        /* base_sql */
        "SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, "
        "nation WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND "
        "o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ("
        "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND "
        "l2.l_suppkey <> l1.l_suppkey) AND NOT EXISTS (SELECT * FROM lineitem l3 "
        "WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND "
        "l3.l_receiptdate > l3.l_commitdate) AND s_nationkey = n_nationkey AND "
        "n_name = 'SAUDI ARABIA' GROUP BY s_name ORDER BY numwait DESC, s_name",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q21_aux('SAUDI ARABIA')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q21_aux('SAUDI ARABIA')",
        /* fallback_sql */
        "SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, nation "
        "WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND o_orderstatus = 'F' "
        "AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ( "
        "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey) "
        "AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey "
        "AND l3.l_suppkey <> l1.l_suppkey AND l3.l_receiptdate > l3.l_commitdate) "
        "AND s_nationkey = n_nationkey AND n_name = $1 "
        "GROUP BY s_name ORDER BY numwait DESC, s_name"
    });

} // namespace
} // namespace accelerator_codegen::aux_state::engine::mysql
