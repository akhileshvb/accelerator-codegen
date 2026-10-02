// MySQL query catalog entry for TPC-H Q9, registered at load time.
#include "catalog/mysql/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

const MysqlQueryCatalogRegistrar kRegisterTpchQ9(MysqlSharedQueryCatalogEntry
    {
        9,
        "tpch",
        "tpch_q9",
        /* base_sql */
        "SELECT nation, o_year, SUM(amount) AS sum_profit FROM ("
        "SELECT n_name AS nation, EXTRACT(year FROM o_orderdate) AS o_year, "
        "l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity AS amount "
        "FROM part, supplier, lineitem, partsupp, orders, nation "
        "WHERE s_suppkey = l_suppkey "
        "AND ps_suppkey = l_suppkey "
        "AND ps_partkey = l_partkey "
        "AND p_partkey = l_partkey "
        "AND o_orderkey = l_orderkey "
        "AND s_nationkey = n_nationkey "
        "AND p_name LIKE '%green%'"
        ") AS profit "
        "GROUP BY nation, o_year "
        "ORDER BY nation, o_year DESC",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q9_aux('green')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q9_aux('green')",
    });

} // namespace
} // namespace accelerator_codegen::aux_state::engine::mysql
