#include "tpch/q13/mysql/aux/module.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace accelerator_codegen::aux_state::query {

// This file intentionally only re-exports the finalize-related helpers in
// a compilation unit that keeps module.cpp focused. Q13's finalization is
// performed by TpchQ13Module::EmitResult and the helper below.
std::vector<TpchQ13OutputRow>
ComposeTpchQ13Output(const std::map<int32_t, int64_t> &histogram) {
  std::vector<TpchQ13OutputRow> rows;
  rows.reserve(histogram.size());
  for (const auto &[count, num] : histogram) {
    rows.push_back({static_cast<int64_t>(count), num});
  }
  std::sort(rows.begin(), rows.end(),
            [](const TpchQ13OutputRow &lhs, const TpchQ13OutputRow &rhs) {
              if (lhs.custdist != rhs.custdist)
                return lhs.custdist > rhs.custdist;
              return lhs.c_count > rhs.c_count;
            });
  return rows;
}

} // namespace accelerator_codegen::aux_state::query
