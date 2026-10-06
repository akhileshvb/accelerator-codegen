#include "tpch/q4/postgres/aux/module.hpp"

#include <algorithm>
#include <utility>
#include <variant>
#include <vector>

namespace accelerator_codegen::aux_state::query {
namespace {

struct Q4OutputRow {
  std::string orderpriority;
  bool priority_is_null = false;
  int64_t order_count = 0;
};

}  // namespace

void FinalizeTpchQ4(const std::vector<int64_t> &counts,
                    const TpchQ4Side &side, core::AuxRowSink &sink) {
  const auto &priority_names = side.priority_names;
  std::vector<Q4OutputRow> rows;
  rows.reserve(priority_names.size());
  for (std::size_t idx = 0; idx < priority_names.size(); ++idx) {
    if (idx >= counts.size() || counts[idx] == 0)
      continue;
    rows.push_back({priority_names[idx],
                    static_cast<int32_t>(idx) == side.null_priority_idx,
                    counts[idx]});
  }
  // ORDER BY o_orderpriority: PostgreSQL sorts NULL last.
  std::sort(rows.begin(), rows.end(),
            [](const Q4OutputRow &a, const Q4OutputRow &b) {
              if (a.priority_is_null != b.priority_is_null)
                return b.priority_is_null;
              return a.orderpriority < b.orderpriority;
            });

  sink.Begin({{{"o_orderpriority", core::AuxType::String, true},
               {"order_count", core::AuxType::Int64, false}}});
  for (const auto &row : rows) {
    sink.WriteRow({{row.priority_is_null
                        ? core::AuxValue::Null(core::AuxType::String)
                        : core::AuxValue::Of(core::AuxType::String,
                                             row.orderpriority),
                    core::AuxValue::Of(core::AuxType::Int64,
                                       row.order_count)}});
  }
  sink.End();
}

}  // namespace accelerator_codegen::aux_state::query
