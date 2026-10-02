// GENERATED QUERY: TPC-H Q6's calibration writes, shared by its engines'
// modules (AuxQueryModule::CalibrationWrites) and registered for engine tools
// (core::CalibrationWritesFor("tpch", 6)).
#pragma once

#include <vector>

#include "tpch/calibration_shapes.hpp"

namespace accelerator_codegen::aux_state::query {

inline std::vector<core::CalibrationWrite> TpchQ6CalibrationWrites() {
  using namespace calibration;
   return Writes("lineitem", "l_extendedprice = l_extendedprice + 1", "l_extendedprice = l_extendedprice - 1", "l_tax = l_tax + 1", "l_tax = l_tax - 1",
                kLineitemShapes);
}

inline const core::CalibrationWritesRegistrar kTpchQ6CalibrationWrites{"tpch", 6, &TpchQ6CalibrationWrites};

} // namespace accelerator_codegen::aux_state::query
