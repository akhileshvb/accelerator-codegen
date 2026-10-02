// GENERATED QUERY: TPC-H Q4's calibration writes, shared by its engines'
// modules (AuxQueryModule::CalibrationWrites) and registered for engine tools
// (core::CalibrationWritesFor("tpch", 4)).
#pragma once

#include <vector>

#include "tpch/calibration_shapes.hpp"

namespace accelerator_codegen::aux_state::query {

inline std::vector<core::CalibrationWrite> TpchQ4CalibrationWrites() {
  using namespace calibration;
   return Writes("lineitem", "l_receiptdate = l_receiptdate + INTERVAL '1' DAY", "l_receiptdate = l_receiptdate - INTERVAL '1' DAY", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1",
                kLineitemShapes);
}

inline const core::CalibrationWritesRegistrar kTpchQ4CalibrationWrites{"tpch", 4, &TpchQ4CalibrationWrites};

} // namespace accelerator_codegen::aux_state::query
