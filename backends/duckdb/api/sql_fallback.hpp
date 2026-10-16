// GENERATED BACKEND
#pragma once

// Answers a query from SQL: the catalog entry's fallback_sql with the call's
// arguments as literals, run on the scan connection (the reader's snapshot
// inside a ScanRedirect).

#include <string>

#include "backends/duckdb/api/catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::engine {

::duckdb::Value AuxValueToDuckdb(const core::AuxValue &value);

// `sql` with each $n replaced by the n-th value as a parenthesized SQL literal
// (highest n first, so $1 never matches inside $10).
std::string WithLiterals(const std::string &sql, const ::duckdb::vector<::duckdb::Value> &values);

// False: the entry has no SQL for these arguments.
bool AnswerFromFallbackSql(::duckdb::Connection &connection, const SharedQueryCatalogEntry &entry,
                           const core::QueryArguments &arguments, core::AuxRowSink &sink);

} // namespace accelerator_codegen::aux_state::engine
