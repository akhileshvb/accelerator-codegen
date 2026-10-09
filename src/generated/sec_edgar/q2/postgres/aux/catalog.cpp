// GENERATED BACKEND — PostgreSQL catalog entry for SEC-EDGAR Q2.
#include "catalog/postgres/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

const PostgresQueryCatalogRegistrar kRegistrar(PostgresSharedQueryCatalogEntry
    {
        2,
        "sec_edgar",
        "sec_edgar_q2",
        /* base_sql */
        "SELECT s.name, n.tag, n.value "
        "FROM num n JOIN sub s ON n.adsh = s.adsh "
        "JOIN (SELECT adsh, tag, MAX(value) AS max_value FROM num "
        "WHERE uom = 'pure' AND value IS NOT NULL GROUP BY adsh, tag) m "
        "ON n.adsh = m.adsh AND n.tag = m.tag AND n.value = m.max_value "
        "WHERE n.uom = 'pure' AND s.fy = 2022 AND n.value IS NOT NULL "
        "ORDER BY n.value DESC, s.name, n.tag LIMIT 100",
        /* accelerator_sql */
        "SELECT * FROM accel_sec_edgar_q2_aux()",
        /* aux_sql */
        "SELECT * FROM accel_sec_edgar_q2_aux(2022, 'pure')",
    }
);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres
