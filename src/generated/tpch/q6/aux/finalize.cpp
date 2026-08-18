#include "tpch/q6/aux/finalize.hpp"

#include <algorithm>

#include "duckdb/common/types/interval.hpp"

namespace accelerator_codegen {
namespace {

#if defined(__SIZEOF_INT128__)
Q6AccT ToNative(int64_t value) {
	return static_cast<Q6AccT>(value);
}

Q6AccT ScaleContribution(int64_t extendedprice_raw, int64_t discount_raw) {
	return ToNative(extendedprice_raw) * ToNative(discount_raw);
}

void AddInPlace(Q6AccT &target, Q6AccT amount, int8_t sign) {
	target += static_cast<Q6AccT>(sign) * amount;
}
#else
Q6AccT ScaleContribution(int64_t extendedprice_raw, int64_t discount_raw) {
	duckdb::hugeint_t price;
	duckdb::hugeint_t discount;
	duckdb::hugeint_t product;
	if (!duckdb::Hugeint::TryConvert(extendedprice_raw, price) || !duckdb::Hugeint::TryConvert(discount_raw, discount) ||
	    !duckdb::Hugeint::TryMultiply(price, discount, product)) {
		return duckdb::Hugeint::Convert(0);
	}
	return product;
}

void AddInPlace(Q6AccT &target, Q6AccT amount, int8_t sign) {
	if (sign >= 0) {
		duckdb::Hugeint::AddInPlace(target, amount);
	} else {
		if (!duckdb::Hugeint::TrySubtractInPlace(target, amount)) {
			throw duckdb::OutOfRangeException("Overflow while updating Q6 revenue");
		}
	}
}
#endif

bool Q6DiscountInRange(int64_t discount_cent) {
	return discount_cent >= 0 && discount_cent < kQ6DiscountBuckets;
}

bool Q6QuantityInRange(int64_t quantity_scaled) {
	const auto bucket = Q6RevenueMatrix::QuantityBucket(quantity_scaled);
	return bucket >= 0 && bucket <= kQ6MaxQuantityBucket;
}

} // namespace

Q6AccT Q6RowContributionRaw(int64_t extendedprice_raw, int64_t discount_raw) {
	return ScaleContribution(extendedprice_raw, discount_raw);
}

void Q6RevenueMatrix::AddCell(int8_t discount_cent, int64_t quantity_scaled, Q6AccT contribution,
                              int8_t sign) {
	if (!Q6DiscountInRange(discount_cent) || !Q6QuantityInRange(quantity_scaled)) {
		return;
	}
	AddInPlace(At(discount_cent, quantity_scaled), contribution, sign);
}

Q6AccT Q6RevenueMatrix::SumRange(int64_t discount_lower_raw, int64_t discount_upper_raw,
                                 int64_t quantity_upper_raw) const {
	return SumQ6MatrixRange(*this, discount_lower_raw, discount_upper_raw, quantity_upper_raw);
}

void ApplyQ6CellContribution(Q6RevenueMatrix &matrix, int8_t discount_cent, int64_t quantity_scaled,
                             Q6AccT contribution, int8_t sign) {
	matrix.AddCell(discount_cent, quantity_scaled, contribution, sign);
}

Q6AccT SumQ6MatrixRange(const Q6RevenueMatrix &matrix, int64_t discount_lower_raw,
                        int64_t discount_upper_raw, int64_t quantity_upper_raw) {
	if (quantity_upper_raw <= 0) {
		return static_cast<Q6AccT>(0);
	}
	const auto quantity_limit = std::min<int32_t>(
	    Q6RevenueMatrix::QuantityBucket(quantity_upper_raw - 1), kQ6MaxQuantityBucket);
	const auto discount_lower = std::max<int64_t>(discount_lower_raw, 0);
	const auto discount_upper = std::min<int64_t>(discount_upper_raw, kQ6MaxDiscountCent);
	if (discount_lower > discount_upper || quantity_limit < 0) {
		return static_cast<Q6AccT>(0);
	}

	Q6AccT total = static_cast<Q6AccT>(0);
	for (int64_t discount_cent = discount_lower; discount_cent <= discount_upper; ++discount_cent) {
		for (int32_t bucket = 0; bucket <= quantity_limit; ++bucket) {
			AddInPlace(total,
			           matrix.At(static_cast<int8_t>(discount_cent),
			                     static_cast<int64_t>(bucket) * kQ6QuantityScale),
			           1);
		}
	}
	return total;
}

duckdb::hugeint_t Q6RevenueFromAccumulator(Q6AccT raw) {
#if defined(__SIZEOF_INT128__)
	return duckdb::hugeint_t(static_cast<int64_t>(raw >> 64), static_cast<uint64_t>(raw));
#else
	return raw;
#endif
}

duckdb::date_t Q6ComputeExclusiveEndDate(duckdb::date_t start_date) {
	duckdb::interval_t one_year;
	one_year.months = duckdb::Interval::MONTHS_PER_YEAR;
	one_year.days = 0;
	one_year.micros = 0;
	return duckdb::Interval::Add(start_date, one_year);
}

} // namespace accelerator_codegen
