// MySQL query catalog entry for TPC-H Q6, registered at load time.
#include "backends/mysql/api/catalog/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

const MysqlQueryCatalogRegistrar kRegisterTpchQ6(MysqlSharedQueryCatalogEntry
    {
        6,
        "tpch",
        "tpch_q6",
        /* base_sql */
        "SELECT SUM(l_extendedprice * l_discount) AS revenue "
        "FROM lineitem "
        "WHERE l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01' "
        "AND l_discount BETWEEN 0.02 AND 0.04 AND l_quantity < 25",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q6_aux(DATE '1995-01-01', 0.03, 25)",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q6_aux(DATE '1995-01-01', 0.03, 25)",
    });

} // namespace
} // namespace accelerator_codegen::aux_state::engine::mysql
