#include "tpch/q21/mysql/aux/module.hpp"

#include <algorithm>
#include <vector>

namespace accelerator_codegen::aux_state::query {

bool TpchQ21BetterOutputRow(const TpchQ21OutputRow &lhs,
                             const TpchQ21OutputRow &rhs) {
  if (lhs.numwait != rhs.numwait)
    return lhs.numwait > rhs.numwait;
  return lhs.name < rhs.name;
}

void TpchQ21FinalizeAndEmit(const TpchQ21State &state,
                             core::AuxRowSink &sink) {
  std::vector<TpchQ21OutputRow> output_rows;

  for (const auto &[nationkey, supp_map] : state.nation_numwait) {
    for (const auto &[suppkey, numwait] : supp_map) {
      if (numwait <= 0)
        continue;
      const auto name_it = state.supplier_names.find(suppkey);
      if (name_it == state.supplier_names.end())
        continue;
      TpchQ21OutputRow row;
      row.name = name_it->second;
      row.numwait = numwait;
      output_rows.push_back(std::move(row));
    }
  }

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