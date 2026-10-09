// GENERATED BACKEND — PostgreSQL catalog entry for SEC-EDGAR Q3.
#include "catalog/postgres/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

const PostgresQueryCatalogRegistrar kRegistrar(PostgresSharedQueryCatalogEntry
    {
        3,
        "sec_edgar",
        "sec_edgar_q3",
        /* base_sql */
        "SELECT s.name, s.cik, SUM(n.value) AS total_value "
        "FROM num n JOIN sub s ON n.adsh = s.adsh "
        "WHERE n.uom = 'USD' AND s.fy = 2022 AND n.value IS NOT NULL "
        "GROUP BY s.name, s.cik "
        "HAVING SUM(n.value) > ("
        "  SELECT AVG(sub_total) FROM ("
        "    SELECT SUM(n2.value) AS sub_total "
        "    FROM num n2 JOIN sub s2 ON n2.adsh = s2.adsh "
        "    WHERE n2.uom = 'USD' AND s2.fy = 2022 AND n2.value IS NOT NULL "
        "    GROUP BY s2.cik) avg_sub) "
        "ORDER BY total_value DESC LIMIT 100",
        /* accelerator_sql */
        "SELECT * FROM accel_sec_edgar_q3_aux()",
        /* aux_sql */
        "SELECT * FROM accel_sec_edgar_q3_aux(2022, 'USD')",
    }
);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres
