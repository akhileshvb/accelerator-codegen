// MySQL query catalog entry for TPC-H Q19, registered at load time.
#include "catalog/mysql/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

const MysqlQueryCatalogRegistrar kRegisterTpchQ19(MysqlSharedQueryCatalogEntry
    {
        19,
        "tpch",
        "tpch_q19",
        /* base_sql */
        "SELECT COALESCE(SUM(l_extendedprice * (1 - l_discount)), 0) AS revenue "
        "FROM lineitem, part "
        "WHERE (p_partkey = l_partkey "
        "AND p_brand = 'Brand#31' "
        "AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG') "
        "AND l_quantity >= 4 AND l_quantity <= 4 + 10 "
        "AND p_size BETWEEN 1 AND 5 "
        "AND l_shipmode IN ('AIR', 'AIR REG') "
        "AND l_shipinstruct = 'DELIVER IN PERSON') "
        "OR (p_partkey = l_partkey "
        "AND p_brand = 'Brand#12' "
        "AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK') "
        "AND l_quantity >= 15 AND l_quantity <= 15 + 10 "
        "AND p_size BETWEEN 1 AND 10 "
        "AND l_shipmode IN ('AIR', 'AIR REG') "
        "AND l_shipinstruct = 'DELIVER IN PERSON') "
        "OR (p_partkey = l_partkey "
        "AND p_brand = 'Brand#51' "
        "AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG') "
        "AND l_quantity >= 23 AND l_quantity <= 23 + 10 "
        "AND p_size BETWEEN 1 AND 15 "
        "AND l_shipmode IN ('AIR', 'AIR REG') "
        "AND l_shipinstruct = 'DELIVER IN PERSON')",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q19_aux('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q19_aux('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)",
    });

} // namespace
} // namespace accelerator_codegen::aux_state::engine::mysql
