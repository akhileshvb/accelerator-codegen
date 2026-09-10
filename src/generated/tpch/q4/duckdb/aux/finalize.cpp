#include "tpch/q4/duckdb/aux/module.hpp"

#include <algorithm>
#include <utility>
#include <variant>
#include <vector>

namespace accelerator_codegen::aux_state::query {
namespace {

struct Q4OutputRow {
  std::string orderpriority;
  int64_t order_count = 0;
};

}  // namespace

void FinalizeTpchQ4(const std::vector<int64_t> &counts,
                    const std::vector<std::string> &priority_names,
                    core::AuxRowSink &sink) {
  std::vector<Q4OutputRow> rows;
  rows.reserve(priority_names.size());
  for (std::size_t idx = 0; idx < priority_names.size(); ++idx) {
    if (idx >= counts.size() || counts[idx] == 0)
      continue;
    rows.push_back({priority_names[idx], counts[idx]});
  }
  std::sort(rows.begin(), rows.end(),
            [](const Q4OutputRow &a, const Q4OutputRow &b) {
              return a.orderpriority < b.orderpriority;
            });

  sink.Begin({{{"o_orderpriority", core::AuxType::String, false},
               {"order_count", core::AuxType::Int64, false}}});
  for (const auto &row : rows) {
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::String, row.orderpriority),
                    core::AuxValue::Of(core::AuxType::Int64,
                                       row.order_count)}});
  }
  sink.End();
}

}  // namespace accelerator_codegen::aux_state::query
