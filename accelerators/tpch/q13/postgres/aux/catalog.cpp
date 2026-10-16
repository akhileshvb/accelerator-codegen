// GENERATED BACKEND — PostgreSQL catalog entry for TPC-H Q13.
#include "backends/postgres/api/catalog/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

const PostgresQueryCatalogRegistrar kRegistrar(PostgresSharedQueryCatalogEntry
    {
        13,
        "tpch",
        "tpch_q13",
        /* base_sql */
        "SELECT c_count, COUNT(*) AS custdist FROM ("
        "SELECT c_custkey, COUNT(o_orderkey) AS c_count FROM customer "
        "LEFT OUTER JOIN orders ON c_custkey = o_custkey AND o_comment NOT LIKE "
        "'%unusual%accounts%' GROUP BY c_custkey) AS c_orders "
        "GROUP BY c_count ORDER BY custdist DESC, c_count DESC",
        /* accelerator_sql */
        "SELECT * FROM accel_tpch_q13('%unusual%accounts%')",
        /* aux_sql */
        "SELECT * FROM accel_tpch_q13_aux('%unusual%accounts%')",
        /* fallback_sql */
        "SELECT c_count, COUNT(*) AS custdist FROM ( "
        "SELECT c_custkey, COUNT(o_orderkey) AS c_count FROM customer "
        "LEFT OUTER JOIN orders ON c_custkey = o_custkey AND o_comment NOT LIKE $1 "
        "GROUP BY c_custkey) AS c_orders "
        "GROUP BY c_count ORDER BY custdist DESC, c_count DESC"
    }
);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres
