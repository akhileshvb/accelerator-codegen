#include "tpch/q17/aux/module.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace accelerator_codegen::aux_state::query {

double TpchQ17FinalizeAvgYearly(int64_t total_extendedprice_raw,
                                bool has_contribution) {
  if (!has_contribution)
    return std::numeric_limits<double>::quiet_NaN();
  return static_cast<double>(total_extendedprice_raw) / 100.0 / 7.0;
}

} // namespace accelerator_codegen::aux_state::query