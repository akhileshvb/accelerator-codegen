// GENERATED BACKEND — PostgreSQL catalog entry for SEC EDGAR Q6.
#include "catalog/postgres/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

const PostgresQueryCatalogRegistrar kRegistrar(PostgresSharedQueryCatalogEntry
    {
        6,
        "sec_edgar",
        "sec_edgar_q6",
        /* base_sql */
        "SELECT s.name, p.stmt, n.tag, p.plabel, "
        "SUM(n.value) AS total_value, COUNT(*) AS cnt "
        "FROM num n "
        "JOIN sub s ON n.adsh = s.adsh "
        "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
        "WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023 "
        "AND n.value IS NOT NULL "
        "GROUP BY s.name, p.stmt, n.tag, p.plabel "
        "ORDER BY total_value DESC, s.name, n.tag, p.plabel "
        "LIMIT 200",
        /* accelerator_sql */
        "SELECT * FROM accel_sec_edgar_q6_aux()",
        /* aux_sql */
        "SELECT * FROM accel_sec_edgar_q6_aux()",
    }
);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres
