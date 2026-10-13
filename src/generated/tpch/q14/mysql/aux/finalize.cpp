#include "tpch/q14/mysql/aux/module.hpp"

#include "accelerator_codegen/stateful/core/mysql_decimal.hpp"
#include "accelerator_codegen/stateful/prims/pg_numeric.hpp"

namespace accelerator_codegen::aux_state::query {

// 100.00 * SUM(promo revenue) / SUM(revenue) as MySQL's DECIMAL arithmetic
// prints it: the numerator has scale 2 + 4 = 6, so the quotient has scale 6 + 4.
// False: NULL (SUM over no rows, or a zero denominator).
bool TpchQ14FinalizePromoRevenue(const TpchQ14DayTotals &totals, std::string &text) {
  if (totals.count == 0)
    return false;
  pg_numeric::Decimal quotient;
  if (!mysql_decimal::Div(totals.promo_revenue_raw * 10000, 6, totals.total_revenue_raw, 4, quotient))
    return false;
  text = pg_numeric::ToText(quotient);
  return true;
}

} // namespace accelerator_codegen::aux_state::query
