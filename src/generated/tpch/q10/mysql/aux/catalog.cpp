// MySQL query catalog entry for TPC-H Q10, registered at load time.
#include "catalog/mysql/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

const MysqlQueryCatalogRegistrar kRegisterTpchQ10(MysqlSharedQueryCatalogEntry
    {
        10,
        "tpch",
        "tpch_q10",
        /* base_sql */
        "SELECT c_custkey, c_name, SUM(l_extendedprice * (1 - l_discount)) AS "
        "revenue, c_acctbal, n_name, c_address, c_phone, c_comment "
        "FROM customer JOIN orders ON c_custkey = o_custkey "
        "JOIN lineitem ON l_orderkey = o_orderkey JOIN nation ON "
        "c_nationkey = n_nationkey "
        "WHERE o_orderdate >= DATE '1993-06-01' "
        "AND o_orderdate < DATE_ADD(DATE '1993-06-01', INTERVAL 3 MONTH) AND "
        "l_returnflag = 'R' "
        "GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, "
        "c_comment ORDER BY revenue DESC, c_custkey LIMIT 20",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q10_aux(DATE '1993-06-01')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q10_aux(DATE '1993-06-01')",
    });

} // namespace
} // namespace accelerator_codegen::aux_state::engine::mysql
