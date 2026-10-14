// MySQL query catalog entry for TPC-H Q5, registered at load time.
#include "backends/mysql/api/catalog/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

const MysqlQueryCatalogRegistrar kRegisterTpchQ5(MysqlSharedQueryCatalogEntry
    {
        5,
        "tpch",
        "tpch_q5",
        /* base_sql */
        "SELECT n_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue "
        "FROM customer, orders, lineitem, supplier, nation, region "
        "WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey "
        "AND l_suppkey = s_suppkey AND c_nationkey = s_nationkey "
        "AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
        "AND r_name = 'AMERICA' "
        "AND o_orderdate >= DATE '1995-01-01' "
        "AND o_orderdate < DATE_ADD(DATE '1995-01-01', INTERVAL 1 YEAR) "
        "GROUP BY n_name ORDER BY revenue DESC",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q5_aux('AMERICA', DATE '1995-01-01')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q5_aux('AMERICA', DATE '1995-01-01')",
    });

} // namespace
} // namespace accelerator_codegen::aux_state::engine::mysql
