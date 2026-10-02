// GENERATED QUERY: TPC-H Q2's calibration writes, shared by its engines'
// modules (AuxQueryModule::CalibrationWrites) and registered for engine tools
// (core::CalibrationWritesFor("tpch", 2)).
#pragma once

#include <vector>

#include "tpch/calibration_shapes.hpp"

namespace accelerator_codegen::aux_state::query {

inline std::vector<core::CalibrationWrite> TpchQ2CalibrationWrites() {
  using namespace calibration;
  // A supplier that is not a part's cheapest (cost > 900, the minimum is
  // almost always lower) changes in place; a cheapest one rebuilds.
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
