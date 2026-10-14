// GENERATED QUERY: TPC-H Q2's calibration writes, shared by its engines'
// modules (AuxQueryModule::CalibrationWrites) and registered for engine tools
// (core::CalibrationWritesFor("tpch", 2)).
#pragma once

#include <vector>

#include "tpch/common/calibration_shapes.hpp"

namespace accelerator_codegen::aux_state::query {

inline std::vector<core::CalibrationWrite> TpchQ2CalibrationWrites() {
  using namespace calibration;
  // Every partsupp change is a point edit of the part's offer list (the
  // cheapest regional offer is derived at query time); cost > 900 keeps the
  // writes away from the cheapest offers so they mostly leave results alone.
  std::vector<Shape> shapes;
  static const std::vector<std::string> wheres = [] {
    std::vector<std::string> out;
    for (const auto &[name, where] : kPartsuppShapes)
      out.push_back(std::string(where) + " AND ps_supplycost > 900");
    return out;
  }();
  for (std::size_t i = 0; i < kPartsuppShapes.size(); ++i)
    shapes.push_back({kPartsuppShapes[i].first, wheres[i].c_str()});
  return Writes("partsupp", "ps_supplycost = ps_supplycost - 1", "ps_supplycost = ps_supplycost + 1",
                "ps_availqty = ps_availqty + 1", "ps_availqty = ps_availqty - 1", shapes);
}

inline const core::CalibrationWritesRegistrar kTpchQ2CalibrationWrites{"tpch", 2, &TpchQ2CalibrationWrites};

} // namespace accelerator_codegen::aux_state::query
