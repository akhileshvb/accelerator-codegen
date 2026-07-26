#include "accelerator_codegen/tpch_q14_finalize.hpp"

#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/value.hpp"

namespace accelerator_codegen {
namespace {

duckdb::hugeint_t AccToHugeint(Q14AccT value) {
#if defined(__SIZEOF_INT128__)
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
#else
	return duckdb::hugeint_t(value);
#endif
}

double DecimalSumToDouble(Q14AccT raw_scale4) {
	auto decimal_value = duckdb::Value::DECIMAL(AccToHugeint(raw_scale4), 38, 4);
	return decimal_value.DefaultCastAs(duckdb::LogicalType::DOUBLE).GetValue<double>();
}

} // namespace

Q14FinalizeResult FinalizeQ14PromoRevenue(bool saw_contribution, Q14AccT total_revenue_raw,
                                          Q14AccT promo_revenue_raw) {
	Q14FinalizeResult result;
	if (!saw_contribution) {
		result.is_null = true;
		return result;
	}
	if (total_revenue_raw == 0) {
		if (promo_revenue_raw == 0) {
			result.value = std::copysign(std::numeric_limits<double>::quiet_NaN(), -1.0);
		} else {
			result.value = std::numeric_limits<double>::infinity();
		}
		return result;
	}
	const Q14AccT numerator_raw = static_cast<Q14AccT>(10000) * promo_revenue_raw;
	const auto numerator_d = duckdb::Value::DECIMAL(AccToHugeint(numerator_raw), 38, 6)
	                             .DefaultCastAs(duckdb::LogicalType::DOUBLE)
	                             .GetValue<double>();
	const auto total_d = DecimalSumToDouble(total_revenue_raw);
	result.value = numerator_d / total_d;
	return result;
}

} // namespace accelerator_codegen
