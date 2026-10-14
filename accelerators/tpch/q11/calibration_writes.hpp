// GENERATED QUERY: TPC-H Q11's calibration writes, shared by its engines'
// modules (AuxQueryModule::CalibrationWrites) and registered for engine tools
// (core::CalibrationWritesFor("tpch", 11)).
#pragma once

#include <vector>

#include "tpch/common/calibration_shapes.hpp"

namespace accelerator_codegen::aux_state::query {

inline std::vector<core::CalibrationWrite> TpchQ11CalibrationWrites() {
  using namespace calibration;
   return Writes("partsupp", "ps_supplycost = ps_supplycost + 1", "ps_supplycost = ps_supplycost - 1", "ps_comment = concat(ps_comment, 'x')", "ps_comment = substr(ps_comment, 1, length(ps_comment) - 1)",
                kPartsuppShapes);
}

inline const core::CalibrationWritesRegistrar kTpchQ11CalibrationWrites{"tpch", 11, &TpchQ11CalibrationWrites};

} // namespace accelerator_codegen::aux_state::query
