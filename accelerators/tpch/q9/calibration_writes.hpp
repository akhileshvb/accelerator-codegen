// GENERATED QUERY: TPC-H Q9's calibration writes, shared by its engines'
// modules (AuxQueryModule::CalibrationWrites) and registered for engine tools
// (core::CalibrationWritesFor("tpch", 9)).
#pragma once

#include <vector>

#include "tpch/common/calibration_shapes.hpp"

namespace accelerator_codegen::aux_state::query {

inline std::vector<core::CalibrationWrite> TpchQ9CalibrationWrites() {
  using namespace calibration;
   return Writes("lineitem", "l_extendedprice = l_extendedprice + 1", "l_extendedprice = l_extendedprice - 1", "l_tax = l_tax + 1", "l_tax = l_tax - 1",
                kLineitemShapes);
}

inline const core::CalibrationWritesRegistrar kTpchQ9CalibrationWrites{"tpch", 9, &TpchQ9CalibrationWrites};

} // namespace accelerator_codegen::aux_state::query
