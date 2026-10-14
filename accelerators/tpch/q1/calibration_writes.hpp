// GENERATED QUERY: TPC-H Q1's calibration writes, shared by its engines'
// modules (AuxQueryModule::CalibrationWrites) and registered for engine tools
// (core::CalibrationWritesFor("tpch", 1)).
#pragma once

#include <vector>

#include "tpch/common/calibration_shapes.hpp"

namespace accelerator_codegen::aux_state::query {

inline std::vector<core::CalibrationWrite> TpchQ1CalibrationWrites() {
  using namespace calibration;
   return Writes("lineitem", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1", "l_suppkey = l_suppkey + 1", "l_suppkey = l_suppkey - 1",
                kLineitemShapes);
}

inline const core::CalibrationWritesRegistrar kTpchQ1CalibrationWrites{"tpch", 1, &TpchQ1CalibrationWrites};

} // namespace accelerator_codegen::aux_state::query
