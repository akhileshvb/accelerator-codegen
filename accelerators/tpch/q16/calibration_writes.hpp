// GENERATED QUERY: TPC-H Q16's calibration writes, shared by its engines'
// modules (AuxQueryModule::CalibrationWrites) and registered for engine tools
// (core::CalibrationWritesFor("tpch", 16)).
#pragma once

#include <vector>

#include "tpch/common/calibration_shapes.hpp"

namespace accelerator_codegen::aux_state::query {

inline std::vector<core::CalibrationWrite> TpchQ16CalibrationWrites() {
  using namespace calibration;
  // supplier comments (complaints) and part / partsupp changes are all maintained in place
  return Writes("supplier", "s_comment = concat(s_comment, 'x')",
                "s_comment = substr(s_comment, 1, length(s_comment) - 1)", "s_acctbal = s_acctbal + 1",
                "s_acctbal = s_acctbal - 1", kSupplierShapes);
}

inline const core::CalibrationWritesRegistrar kTpchQ16CalibrationWrites{"tpch", 16, &TpchQ16CalibrationWrites};

} // namespace accelerator_codegen::aux_state::query
