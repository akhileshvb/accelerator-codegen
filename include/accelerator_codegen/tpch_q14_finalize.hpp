#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace accelerator_codegen {

#if defined(__SIZEOF_INT128__)
using Q14AccT = __int128_t;
#else
using Q14AccT = int64_t;
#endif

struct Q14FinalizeResult {
	bool is_null = false;
	double value = 0.0;
};

Q14FinalizeResult FinalizeQ14PromoRevenue(bool saw_contribution, Q14AccT total_revenue_raw, Q14AccT promo_revenue_raw);

} // namespace accelerator_codegen
