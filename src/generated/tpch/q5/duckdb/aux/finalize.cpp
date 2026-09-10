#include "tpch/q5/duckdb/aux/module.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace accelerator_codegen::aux_state::query {

void TpchQ5FinalizeAndEmit(
    const std::unordered_map<int32_t, std::string> &nation_name,
    const std::unordered_map<int32_t, int32_t> &nation_region,
    int32_t target_regionkey,
    const std::unordered_map<int32_t, int64_t> &revenue_by_nation,
    core::AuxRowSink &sink) {
  sink.Begin({{{"n_name", core::AuxType::String, false},
               {"revenue", core::AuxType::ScaledDecimal, false}}});

  std::vector<std::pair<std::string, int64_t>> rows;
  for (const auto &[nationkey, revenue] : revenue_by_nation) {
    if (revenue == 0)
      continue;
    auto region_it = nation_region.find(nationkey);
    if (region_it == nation_region.end() ||
        region_it->second != target_regionkey)
      continue;
    auto name_it = nation_name.find(nationkey);
    if (name_it == nation_name.end())
      continue;
    rows.emplace_back(name_it->second, revenue);
  }

  // Base Q5 sorts by revenue descending, tie-break not specified in the
  // kernel (stable order depends on nationkey iteration). Use revenue
  // descending then name ascending for deterministic, SQL-compatible output.
  std::sort(rows.begin(), rows.end(),
            [](const std::pair<std::string, int64_t> &a,
               const std::pair<std::string, int64_t> &b) {
              if (a.second != b.second)
                return a.second > b.second;
              return a.first < b.first;
            });

  for (const auto &row : rows) {
    sink.WriteRow(
        {{core::AuxValue::Of(core::AuxType::String, row.first),
          core::AuxValue::Of(core::AuxType::ScaledDecimal,
                             core::ScaledDecimal{row.second, 4})}});
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
