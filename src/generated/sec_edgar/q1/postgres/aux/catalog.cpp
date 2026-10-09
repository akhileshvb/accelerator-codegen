// PostgreSQL catalog entry for SEC EDGAR Q1 (registered into libaccelerator_codegen).
#include "catalog/postgres/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

const PostgresQueryCatalogRegistrar kRegistrar(PostgresSharedQueryCatalogEntry{
    1,
    "sec_edgar",
    "sec_edgar_q1",
    /* base_sql */
    "SELECT stmt, rfile, COUNT(*) AS cnt, COUNT(DISTINCT adsh) AS num_filings, "
    "AVG(line) AS avg_line_num "
    "FROM pre WHERE stmt IS NOT NULL GROUP BY stmt, rfile ORDER BY cnt DESC",
    /* accelerator_sql */
    "SELECT * FROM accel_sec_edgar_q1_aux()",
    /* aux_sql */
    "SELECT * FROM accel_sec_edgar_q1_aux()",
});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres
