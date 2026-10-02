// GENERATED BACKEND — PostgreSQL catalog entry for TPC-H Q21.
#include "catalog/postgres/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

const PostgresQueryCatalogRegistrar kRegistrar(PostgresSharedQueryCatalogEntry
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
        "SELECT * FROM accel_tpch_q21('SAUDI ARABIA')",
        /* aux_sql */
        "SELECT * FROM accel_tpch_q21_aux('SAUDI ARABIA')",
    }
);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres
