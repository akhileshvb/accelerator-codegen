#pragma once

#include <functional>

#include "sec_edgar/q1/common/module.hpp"

namespace accelerator_codegen::aux_state::query {

// DuckDB helper for SecEdgarQ1InitialScan::ScanPreChunks/ScanPreInto: streams
// (stmt, rfile, adsh, line) straight from table storage (no result
// materialization/copy). Begins/ends a transaction on `connection` only when
// none is active.
bool ScanSecEdgarQ1PreNative(
    ::duckdb::Connection &connection,
    const std::function<void(::duckdb::DataChunk &)> &consumer);

} // namespace accelerator_codegen::aux_state::query
