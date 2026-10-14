#include "tpch/q14/postgres/aux/module.hpp"

#include <cmath>
#include <limits>

namespace accelerator_codegen::aux_state::query {

double TpchQ14FinalizePromoRevenue(const TpchQ14DayTotals &totals) {
  if (totals.count == 0)
    return std::numeric_limits<double>::quiet_NaN(); // SUM over no rows: NULL
  if (totals.total_revenue_raw == 0) {
    if (totals.promo_revenue_raw == 0)
      return std::copysign(std::numeric_limits<double>::quiet_NaN(), -1.0);
    return std::numeric_limits<double>::infinity();
  }
  // 100.00 * promo / total from the exact scale-4 sums.
  return static_cast<double>(
      static_cast<long double>(totals.promo_revenue_raw) * 100.0L /
      static_cast<long double>(totals.total_revenue_raw));
}

} // namespace accelerator_codegen::aux_state::query
