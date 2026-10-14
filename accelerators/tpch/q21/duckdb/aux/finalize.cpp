#include "tpch/q21/duckdb/aux/module.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace accelerator_codegen::aux_state::query {

bool TpchQ21BetterOutputRow(const TpchQ21OutputRow &lhs,
                             const TpchQ21OutputRow &rhs) {
  if (lhs.numwait != rhs.numwait)
    return lhs.numwait > rhs.numwait;
  return lhs.name < rhs.name;
}

// Emits the merged (GROUP BY s_name) answer over every nation's rows.
void TpchQ21FinalizeAndEmit(const TpchQ21State &state,
                             core::AuxRowSink &sink) {
  // GROUP BY s_name: suppliers sharing a name form one group.
  std::map<std::string, int64_t> by_name;
  for (const auto &[nationkey, rows] : state.nations) {
    (void)nationkey;
    for (const auto &row : rows->rows)
      by_name[*row.name] += row.numwait;
  }
  std::vector<TpchQ21OutputRow> output_rows;
  output_rows.reserve(by_name.size());
  for (auto &[name, numwait] : by_name)
    output_rows.push_back({name, numwait});
  std::sort(output_rows.begin(), output_rows.end(), TpchQ21BetterOutputRow);

  sink.Begin(core::OutputSchema{});
  for (const auto &row : output_rows) {
    core::AuxRow out;
    out.values.reserve(2);
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, row.name));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.numwait));
    sink.WriteRow(out);
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
