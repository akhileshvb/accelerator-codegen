// MySQL query catalog entry for TPC-H Q15, registered at load time.
#include "catalog/mysql/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

const MysqlQueryCatalogRegistrar kRegisterTpchQ15(MysqlSharedQueryCatalogEntry
    {
        15,
        "tpch",
        "tpch_q15",
        /* base_sql */
        "SELECT s_suppkey, s_name, s_address, s_phone, total_revenue "
        "FROM supplier, ("
        "SELECT l_suppkey AS supplier_no, "
        "SUM(l_extendedprice * (1 - l_discount)) AS total_revenue "
        "FROM lineitem WHERE l_shipdate >= DATE '1994-11-01' "
        "AND l_shipdate < DATE_ADD(DATE '1994-11-01', INTERVAL 3 MONTH) "
        "GROUP BY l_suppkey) revenue0 "
        "WHERE s_suppkey = supplier_no "
        "AND total_revenue = (SELECT MAX(total_revenue) FROM ("
        "SELECT l_suppkey AS supplier_no, "
        "SUM(l_extendedprice * (1 - l_discount)) AS total_revenue "
        "FROM lineitem WHERE l_shipdate >= DATE '1994-11-01' "
        "AND l_shipdate < DATE_ADD(DATE '1994-11-01', INTERVAL 3 MONTH) "
        "GROUP BY l_suppkey) revenue1) ORDER BY s_suppkey",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q15_aux(DATE '1994-11-01')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q15_aux(DATE '1994-11-01')",
    });

} // namespace
} // namespace accelerator_codegen::aux_state::engine::mysql
