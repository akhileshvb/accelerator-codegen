// GENERATED QUERY: TPC-H Q20's calibration writes, shared by its engines'
// modules (AuxQueryModule::CalibrationWrites) and registered for engine tools
// (core::CalibrationWritesFor("tpch", 20)).
#pragma once

#include <vector>

#include "tpch/common/calibration_shapes.hpp"

namespace accelerator_codegen::aux_state::query {

inline std::vector<core::CalibrationWrite> TpchQ20CalibrationWrites() {
  using namespace calibration;
   return Writes("lineitem", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1", "l_extendedprice = l_extendedprice + 1", "l_extendedprice = l_extendedprice - 1",
                kLineitemShapes);
}

inline const core::CalibrationWritesRegistrar kTpchQ20CalibrationWrites{"tpch", 20, &TpchQ20CalibrationWrites};

} // namespace accelerator_codegen::aux_state::query
