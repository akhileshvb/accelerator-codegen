// GENERATED QUERY: TPC-H Q21's calibration writes, shared by its engines'
// modules (AuxQueryModule::CalibrationWrites) and registered for engine tools
// (core::CalibrationWritesFor("tpch", 21)).
#pragma once

#include <vector>

#include "tpch/common/calibration_shapes.hpp"

namespace accelerator_codegen::aux_state::query {

inline std::vector<core::CalibrationWrite> TpchQ21CalibrationWrites() {
  using namespace calibration;
   return Writes("lineitem", "l_receiptdate = l_receiptdate + INTERVAL '1' DAY", "l_receiptdate = l_receiptdate - INTERVAL '1' DAY", "l_quantity = l_quantity + 1", "l_quantity = l_quantity - 1",
                kLineitemShapes);
}

inline const core::CalibrationWritesRegistrar kTpchQ21CalibrationWrites{"tpch", 21, &TpchQ21CalibrationWrites};

} // namespace accelerator_codegen::aux_state::query
