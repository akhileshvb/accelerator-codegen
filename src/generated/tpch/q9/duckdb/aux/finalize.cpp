#include "tpch/q9/duckdb/aux/module.hpp"

#include <algorithm>
#include <vector>

namespace accelerator_codegen::aux_state::query {

// finalize.cpp — Fixed-point nation-year finalization and output ordering.
// This file provides a helper that converts raw profit aggregates into the
// final Q9 output format. It is used by both the DuckDB and PostgreSQL paths.
//
// The core logic lives in TpchQ9Module::EmitResult, but this file may be
// extended with encode/decode helpers or targeted unit-test entry points.

// Sorting comparator for TpchQ9OutputRow: nation ASC, year DESC
bool Q9OutputRowSorter(const TpchQ9OutputRow &lhs,
                       const TpchQ9OutputRow &rhs) {
  if (lhs.nation != rhs.nation)
    return lhs.nation < rhs.nation;
  return lhs.year > rhs.year;
}

} // namespace accelerator_codegen::aux_state::query
