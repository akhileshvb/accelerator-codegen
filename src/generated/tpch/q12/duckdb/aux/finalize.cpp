#include "tpch/q12/duckdb/aux/module.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace accelerator_codegen::aux_state::query {

void TpchQ12FinalizeAndEmit(const std::vector<const TpchQ12DayMap *> &days,
                            const std::string &shipmode1,
                            const std::string &shipmode2,
                            core::AuxRowSink &sink) {
  std::unordered_map<std::string, TpchQ12DayShipmodeStats> merged;
  TpchQ12DayShipmodeStats s1, s2;
  for (const auto *day : days) {
    if (!day)
      continue;
    auto it = day->find(shipmode1);
    if (it != day->end()) {
      s1.high_line_count += it->second.high_line_count;
      s1.low_line_count += it->second.low_line_count;
    }
    if (shipmode2 != shipmode1) {
      it = day->find(shipmode2);
      if (it != day->end()) {
        s2.high_line_count += it->second.high_line_count;
        s2.low_line_count += it->second.low_line_count;
      }
    }
  }
  merged[shipmode1] = s1;
  if (shipmode2 != shipmode1)
    merged[shipmode2] = s2;
  std::vector<std::string> shipmodes;
  const bool modes_equal = shipmode1 == shipmode2;
  if (merged.count(shipmode1) &&
      (merged[shipmode1].high_line_count != 0 ||
       merged[shipmode1].low_line_count != 0))
    shipmodes.push_back(shipmode1);
  if (!modes_equal && merged.count(shipmode2) &&
      (merged[shipmode2].high_line_count != 0 ||
       merged[shipmode2].low_line_count != 0))
    shipmodes.push_back(shipmode2);
  std::sort(shipmodes.begin(), shipmodes.end());
  sink.Begin({{{"l_shipmode", core::AuxType::String, false},
               {"high_line_count", core::AuxType::Int64, false},
               {"low_line_count", core::AuxType::Int64, false}}});
  for (const auto &shipmode : shipmodes) {
    const auto &stats = merged.at(shipmode);
    sink.WriteRow(
        {{core::AuxValue::Of(core::AuxType::String, shipmode),
          core::AuxValue::Of(core::AuxType::Int64, stats.high_line_count),
          core::AuxValue::Of(core::AuxType::Int64, stats.low_line_count)}});
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
