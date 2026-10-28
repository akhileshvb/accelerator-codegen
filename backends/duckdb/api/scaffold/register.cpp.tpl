// SCAFFOLD (pipeline): {{benchmark_name}} Q{{query_number}} on {{engine_name}}, registered with no accelerator behind it.
// Every call to {{table_function}} declines and is answered by the fallback SQL. The implementation replaces this
// file with the query's real registration: its catalog entry (probes, benchmark arguments, cost model), its
// AuxQueryRegistrar and its module (backends/{{engine_name}}/api/README.md, registration role).
#include "backends/duckdb/api/declining_query.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

const DecliningQueryRegistrar kScaffold{DecliningQuery{
    {{query_number}},
    "{{benchmark_name}}",
    "{{accelerator_id}}",
    "{{table_function}}",
    {{argument_types}},
    R"SQL({{base_sql}})SQL",
    R"SQL({{fallback_sql}})SQL",
    {{tables}},
    {{binding}}}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
