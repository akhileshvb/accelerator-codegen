#include "tpch/q14/aux/module.hpp"

#include <cmath>
#include <limits>

namespace accelerator_codegen::aux_state::query {

double TpchQ14FinalizePromoRevenue(const TpchQ14MonthTotals &totals) {
  if (!totals.saw_contribution)
    return std::numeric_limits<double>::quiet_NaN();
  if (totals.total_revenue_raw == 0) {
    if (totals.promo_revenue_raw == 0)
      return std::copysign(std::numeric_limits<double>::quiet_NaN(), -1.0);
    return std::numeric_limits<double>::infinity();
  }
  const double numerator =
      100.0 * static_cast<double>(totals.promo_revenue_raw) / 10000.0;
  const double denominator =
      static_cast<double>(totals.total_revenue_raw) / 10000.0;
  return numerator / denominator;
}

} // namespace accelerator_codegen::aux_state::query
