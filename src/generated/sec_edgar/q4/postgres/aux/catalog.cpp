// GENERATED BACKEND — PostgreSQL catalog entry for SEC-EDGAR Q4.
#include "catalog/postgres/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

const PostgresQueryCatalogRegistrar kRegistrar(PostgresSharedQueryCatalogEntry
    {
        4,
        "sec_edgar",
        "sec_edgar_q4",
        /* base_sql */
        "SELECT s.sic, t.tlabel, p.stmt, "
        "COUNT(DISTINCT s.cik) AS num_companies, "
        "SUM(n.value) AS total_value, "
        "AVG(n.value) AS avg_value "
        "FROM num n "
        "JOIN sub s ON n.adsh = s.adsh "
        "JOIN tag t ON n.tag = t.tag AND n.version = t.version "
        "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
        "WHERE n.uom = 'USD' AND p.stmt = 'EQ' "
        "AND s.sic BETWEEN 4000 AND 4999 "
        "AND n.value IS NOT NULL AND t.abstract = 0 "
        "GROUP BY s.sic, t.tlabel, p.stmt "
        "HAVING COUNT(DISTINCT s.cik) >= 2 "
        "ORDER BY total_value DESC "
        "LIMIT 500",
        /* accelerator_sql */
        "SELECT * FROM accel_sec_edgar_q4_aux()",
        /* aux_sql */
        "SELECT * FROM accel_sec_edgar_q4_aux()",
    }
);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres
