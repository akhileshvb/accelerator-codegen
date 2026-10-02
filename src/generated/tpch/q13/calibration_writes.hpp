// GENERATED QUERY: TPC-H Q13's calibration writes, shared by its engines'
// modules (AuxQueryModule::CalibrationWrites) and registered for engine tools
// (core::CalibrationWritesFor("tpch", 13)).
#pragma once

#include <vector>

#include "tpch/calibration_shapes.hpp"

namespace accelerator_codegen::aux_state::query {

inline std::vector<core::CalibrationWrite> TpchQ13CalibrationWrites() {
  using namespace calibration;
   return Writes("orders", "o_custkey = o_custkey + 1", "o_custkey = o_custkey - 1", "o_totalprice = o_totalprice + 1", "o_totalprice = o_totalprice - 1",
                kOrdersShapes);
}

inline const core::CalibrationWritesRegistrar kTpchQ13CalibrationWrites{"tpch", 13, &TpchQ13CalibrationWrites};

} // namespace accelerator_codegen::aux_state::query
