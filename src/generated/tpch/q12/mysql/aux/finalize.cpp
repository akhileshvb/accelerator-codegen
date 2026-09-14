#include "tpch/q12/mysql/aux/module.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace accelerator_codegen::aux_state::query {

void TpchQ12FinalizeAndEmit(const TpchQ12State &state, int32_t start_days,
                            int32_t end_days, const std::string &shipmode1,
                            const std::string &shipmode2,
                            core::AuxRowSink &sink) {
  std::unordered_map<std::string, TpchQ12DayShipmodeStats> merged;
  for (int32_t day = start_days; day < end_days; ++day) {
    const auto it = state.day_partitions.find(day);
    if (it == state.day_partitions.end())
      continue;
    for (const auto &entry : it->second) {
      if (entry.first != shipmode1 && entry.first != shipmode2)
        continue;
      auto &stats = merged[entry.first];
      stats.high_line_count += entry.second.high_line_count;
      stats.low_line_count += entry.second.low_line_count;
    }
  }
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
