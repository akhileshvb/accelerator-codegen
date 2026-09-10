#include "tpch/q11/postgres/aux/module.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace accelerator_codegen::aux_state::query {

std::vector<TpchQ11OutputRow> FinalizeQ11FromSorted(
    const std::vector<TpchQ11OutputRow> &sorted_rows,
    Q11AccT total_value, double fraction) {
  std::vector<TpchQ11OutputRow> output;

  // Compute threshold in integer domain:
  //   row.value / 100.0 > (total / 100.0) * fraction
  // ⇔ row.value > total * fraction   (100.0 cancels)
  Q11AccT threshold_raw = 0;
#if defined(__SIZEOF_INT128__)
  {
    double total_as_double = static_cast<double>(static_cast<int64_t>(
        static_cast<uint64_t>(total_value)));
    threshold_raw = static_cast<Q11AccT>(total_as_double * fraction);
  }
#else
  threshold_raw = static_cast<Q11AccT>(
      static_cast<double>(total_value) * fraction);
#endif

  // sorted_rows is pre-sorted by value DESC, partkey ASC
  // Iterate and emit while above threshold; break early when below
  for (const auto &row : sorted_rows) {
    if (row.value > threshold_raw) {
      output.push_back(row);
    } else {
      // Since sorted by value DESC, remaining entries are below threshold
      break;
    }
  }

  return output;
}

} // namespace accelerator_codegen::aux_state::query