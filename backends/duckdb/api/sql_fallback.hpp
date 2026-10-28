// GENERATED BACKEND
#pragma once

// Answers a query from SQL: the catalog entry's fallback_sql with the call's
// arguments as literals, run on the scan connection (the reader's snapshot
// inside a ScanRedirect).

#include <optional>
#include <string>

#include "backends/duckdb/api/catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::engine {

::duckdb::Value AuxValueToDuckdb(const core::AuxValue &value);

// ACCEL_FALLBACK_TRAP=1 (the hit contract, host_contract.md): a query's fallback SQL raises
// instead of running, so a read that was meant to hit the state cannot silently fall back.
bool FallbackTrapped();
// Overrides the environment for this process (a harness that lets one read fall back); `std::nullopt` restores it.
void SetFallbackTrap(std::optional<bool> trapped);

// `sql` with each $n replaced by the n-th value as a parenthesized SQL literal
// (highest n first, so $1 never matches inside $10).
std::string WithLiterals(const std::string &sql, const ::duckdb::vector<::duckdb::Value> &values);
// The same for a call's arguments: how every SQL template of a catalog entry is bound.
std::string BindArguments(const std::string &sql, const core::QueryArguments &arguments);

// The SQL that answers the query for these arguments (the base query when
// there are none, for an entry with a default binding), empty if the entry has none.
// Throws when FallbackTrapped().
std::string FallbackSql(const SharedQueryCatalogEntry &entry, const core::QueryArguments &arguments);

// False: the entry has no SQL for these arguments.
bool AnswerFromFallbackSql(::duckdb::Connection &connection, const SharedQueryCatalogEntry &entry,
                           const core::QueryArguments &arguments, core::AuxRowSink &sink);

} // namespace accelerator_codegen::aux_state::engine
