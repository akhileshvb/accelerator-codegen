// MySQL query catalog entry for TPC-H Q8, registered at load time.
#include "backends/mysql/api/catalog/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

const MysqlQueryCatalogRegistrar kRegisterTpchQ8(MysqlSharedQueryCatalogEntry
    {
        8,
        "tpch",
        "tpch_q8",
        /* base_sql */
        "SELECT o_year, SUM(CASE WHEN nation = 'INDIA' THEN volume ELSE 0 END) / "
        "SUM(volume) AS mkt_share FROM ("
        "SELECT EXTRACT(year FROM o_orderdate) AS o_year, "
        "l_extendedprice * (1 - l_discount) AS volume, n2.n_name AS nation "
        "FROM part, supplier, lineitem, orders, customer, nation n1, nation n2, region "
        "WHERE p_partkey = l_partkey AND s_suppkey = l_suppkey "
        "AND l_orderkey = o_orderkey AND o_custkey = c_custkey "
        "AND c_nationkey = n1.n_nationkey AND n1.n_regionkey = r_regionkey "
        "AND r_name = 'ASIA' AND s_nationkey = n2.n_nationkey "
        "AND o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' "
        "AND p_type = 'PROMO BRUSHED BRASS') AS all_nations "
        "GROUP BY o_year ORDER BY o_year",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q8_aux('INDIA', 'ASIA', 'PROMO BRUSHED BRASS')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q8_aux('INDIA', 'ASIA', 'PROMO BRUSHED BRASS')",
    });

} // namespace
} // namespace accelerator_codegen::aux_state::engine::mysql
