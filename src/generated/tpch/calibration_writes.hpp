// GENERATED: TPC-H calibration writes (core::CalibrationWrite), one set per
// query, shared by every engine (ANSI SQL).
#pragma once

#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::query {

// Writes of increasing size and spread for TPC-H query `query` (1-22): a
// column the accelerator depends on changes, over ranges of its table; the
// control writes the same rows on a column it does not read. Empty for an
// unknown query.
std::vector<core::CalibrationWrite> TpchCalibrationWrites(int query);

} // namespace accelerator_codegen::aux_state::query
