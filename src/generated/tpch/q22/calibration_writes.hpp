// GENERATED QUERY: TPC-H Q22's calibration writes, shared by its engines'
// modules (AuxQueryModule::CalibrationWrites) and registered for engine tools
// (core::CalibrationWritesFor("tpch", 22)).
#pragma once

#include <vector>

#include "tpch/calibration_shapes.hpp"

namespace accelerator_codegen::aux_state::query {

inline std::vector<core::CalibrationWrite> TpchQ22CalibrationWrites() {
  using namespace calibration;
   return Writes("customer", "c_acctbal = c_acctbal + 1", "c_acctbal = c_acctbal - 1", "c_nationkey = c_nationkey + 1", "c_nationkey = c_nationkey - 1",
                kCustomerShapes);
}

inline const core::CalibrationWritesRegistrar kTpchQ22CalibrationWrites{"tpch", 22, &TpchQ22CalibrationWrites};

} // namespace accelerator_codegen::aux_state::query
