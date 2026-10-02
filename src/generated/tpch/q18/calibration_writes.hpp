// GENERATED QUERY: TPC-H Q18's calibration writes, shared by its engines'
// modules (AuxQueryModule::CalibrationWrites) and registered for engine tools
// (core::CalibrationWritesFor("tpch", 18)).
#pragma once

#include <vector>

#include "tpch/calibration_shapes.hpp"

namespace accelerator_codegen::aux_state::query {

inline std::vector<core::CalibrationWrite> TpchQ18CalibrationWrites() {
  using namespace calibration;
   return Writes("lineitem", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1", "l_tax = l_tax + 1", "l_tax = l_tax - 1",
                kLineitemShapes);
}

inline const core::CalibrationWritesRegistrar kTpchQ18CalibrationWrites{"tpch", 18, &TpchQ18CalibrationWrites};

} // namespace accelerator_codegen::aux_state::query
