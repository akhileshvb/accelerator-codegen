// GENERATED BACKEND — PostgreSQL catalog entry for TPC-H Q2.
#include "backends/postgres/api/catalog/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

const PostgresQueryCatalogRegistrar kRegistrar(PostgresSharedQueryCatalogEntry
    {
        2,
        "tpch",
        "tpch_q2",
        /* base_sql */
        "SELECT s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, s_phone, s_comment "
        "FROM part, supplier, partsupp, nation, region "
        "WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND p_size = 15 "
        "AND p_type LIKE '%BRASS' AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
        "AND r_name = 'EUROPE' "
        "AND ps_supplycost = (SELECT MIN(ps_supplycost) FROM partsupp, supplier, nation, region "
        "WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND s_nationkey = n_nationkey "
        "AND n_regionkey = r_regionkey AND r_name = 'EUROPE') "
        "ORDER BY s_acctbal DESC, n_name, s_name, p_partkey LIMIT 100",
        /* accelerator_sql */
        "SELECT * FROM accel_tpch_q2(15, '%BRASS', 'EUROPE')",
        /* aux_sql */
        "SELECT * FROM accel_tpch_q2_aux(15, '%BRASS', 'EUROPE')",
        /* fallback_sql */
        "SELECT s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, s_phone, s_comment "
        "FROM part, supplier, partsupp, nation, region "
        "WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND p_size = $1 "
        "AND p_type LIKE $2 AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
        "AND r_name = $3 "
        "AND ps_supplycost = (SELECT MIN(ps_supplycost) FROM partsupp, supplier, nation, region "
        "WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey "
        "AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey AND r_name = $3) "
        "ORDER BY s_acctbal DESC, n_name, s_name, p_partkey LIMIT 100"
    }
);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres
