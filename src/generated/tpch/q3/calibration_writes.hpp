// GENERATED QUERY: TPC-H Q3's calibration writes, shared by its engines'
// modules (AuxQueryModule::CalibrationWrites) and registered for engine tools
// (core::CalibrationWritesFor("tpch", 3)).
#pragma once

#include <vector>

#include "tpch/calibration_shapes.hpp"

namespace accelerator_codegen::aux_state::query {

inline std::vector<core::CalibrationWrite> TpchQ3CalibrationWrites() {
  using namespace calibration;
   return Writes("lineitem", "l_extendedprice = l_extendedprice + 1", "l_extendedprice = l_extendedprice - 1", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1",
                kLineitemShapes);
}

inline const core::CalibrationWritesRegistrar kTpchQ3CalibrationWrites{"tpch", 3, &TpchQ3CalibrationWrites};

} // namespace accelerator_codegen::aux_state::query
