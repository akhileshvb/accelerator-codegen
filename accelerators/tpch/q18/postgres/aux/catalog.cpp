// GENERATED BACKEND — PostgreSQL catalog entry for TPC-H Q18.
#include "backends/postgres/api/catalog/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

const PostgresQueryCatalogRegistrar kRegistrar(PostgresSharedQueryCatalogEntry
    {
        18,
        "tpch",
        "tpch_q18",
        /* base_sql */
        "SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, "
        "SUM(l_quantity) AS sum_qty "
        "FROM customer, orders, lineitem "
        "WHERE o_orderkey IN ("
        "SELECT l_orderkey FROM lineitem GROUP BY l_orderkey "
        "HAVING SUM(l_quantity) > 312) "
        "AND c_custkey = o_custkey AND o_orderkey = l_orderkey "
        "GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice "
        "ORDER BY o_totalprice DESC, o_orderdate ASC",
        /* accelerator_sql */
        "SELECT * FROM accel_tpch_q18(312)",
        /* aux_sql */
        "SELECT * FROM accel_tpch_q18_aux(312)",
        /* fallback_sql */
        "SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, SUM(l_quantity) AS sum_qty "
        "FROM customer, orders, lineitem "
        "WHERE o_orderkey IN ( "
        "SELECT l_orderkey FROM lineitem GROUP BY l_orderkey HAVING SUM(l_quantity) > $1::numeric) "
        "AND c_custkey = o_custkey AND o_orderkey = l_orderkey "
        "GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice "
        "ORDER BY o_totalprice DESC, o_orderdate ASC"
    }
);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres
