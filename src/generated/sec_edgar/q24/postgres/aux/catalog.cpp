// GENERATED BACKEND — PostgreSQL catalog entry for SEC-EDGAR Q24.
#include "catalog/postgres/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

const PostgresQueryCatalogRegistrar kRegistrar(PostgresSharedQueryCatalogEntry
    {
        24,
        "sec_edgar",
        "sec_edgar_q24",
        /* base_sql */
        "SELECT n.tag, n.version, COUNT(*) AS cnt, SUM(n.value) AS total "
        "FROM num n "
        "LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version AND "
        "n.adsh = p.adsh "
        "WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 AND 20231231 "
        "AND n.value IS NOT NULL AND p.adsh IS NULL "
        "GROUP BY n.tag, n.version "
        "HAVING COUNT(*) > 10 "
        "ORDER BY cnt DESC "
        "LIMIT 100",
        /* accelerator_sql (no stateless kernel on PostgreSQL: the aux path) */
        "SELECT * FROM accel_sec_edgar_q24_aux()",
        /* aux_sql */
        "SELECT * FROM accel_sec_edgar_q24_aux()",
    }
);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres
