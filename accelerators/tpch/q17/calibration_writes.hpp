// GENERATED QUERY: TPC-H Q17's calibration writes, shared by its engines'
// modules (AuxQueryModule::CalibrationWrites) and registered for engine tools
// (core::CalibrationWritesFor("tpch", 17)).
#pragma once

#include <vector>

#include "tpch/common/calibration_shapes.hpp"

namespace accelerator_codegen::aux_state::query {

inline std::vector<core::CalibrationWrite> TpchQ17CalibrationWrites() {
  using namespace calibration;
   return Writes("lineitem", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1", "l_tax = l_tax + 1", "l_tax = l_tax - 1",
                kLineitemShapes);
}

inline const core::CalibrationWritesRegistrar kTpchQ17CalibrationWrites{"tpch", 17, &TpchQ17CalibrationWrites};

} // namespace accelerator_codegen::aux_state::query
