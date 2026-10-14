#include "tpch/q6/duckdb/aux/finalize.hpp"

#include <algorithm>
#if defined(__AVX512F__)
#include <immintrin.h>
#endif

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

// dst[i] += src[i] for 128-bit accumulators (wraps mod 2^128, identical to the
// scalar sum). AVX-512: four accumulators per vector; the carry out of each low
// 64-bit lane is added to the matching high lane.
void AddQ6Block(Q6AccT *dst, const Q6AccT *src, std::size_t count) {
	std::size_t i = 0;
#if defined(__SIZEOF_INT128__) && defined(__AVX512F__)
	const __m512i one = _mm512_set1_epi64(1);
	for (; i + 4 <= count; i += 4) {
		const __m512i a = _mm512_loadu_si512(reinterpret_cast<const void *>(dst + i));
		const __m512i b = _mm512_loadu_si512(reinterpret_cast<const void *>(src + i));
		const __m512i sum = _mm512_add_epi64(a, b);
		const __mmask8 carry = _mm512_cmplt_epu64_mask(sum, a) & 0x55;
		_mm512_storeu_si512(reinterpret_cast<void *>(dst + i),
		                    _mm512_mask_add_epi64(sum, static_cast<__mmask8>(carry << 1), sum, one));
	}
#endif
	for (; i < count; ++i)
		AddInPlace(dst[i], src[i], 1);
}

bool Q6DiscountInRange(int64_t discount_cent) {
	return discount_cent >= 0 && discount_cent < kQ6DiscountBuckets;
}

bool Q6QuantityInRange(int64_t quantity_scaled) {
	const auto bucket = Q6RevenueMatrix::QuantityBucket(quantity_scaled);
	return bucket >= 0 && bucket <= kQ6MaxQuantityBucket;
}

} // namespace

bool Q6InDomain(int64_t discount_raw, int64_t quantity_raw) {
	return discount_raw >= 0 && discount_raw <= kQ6MaxDiscountCent && quantity_raw >= 0 &&
	       quantity_raw <= int64_t {kQ6MaxQuantityBucket} * kQ6QuantityScale && quantity_raw % kQ6QuantityScale == 0;
}

Q6AccT Q6RowContributionRaw(int64_t extendedprice_raw, int64_t discount_raw) {
	return ScaleContribution(extendedprice_raw, discount_raw);
}

void Q6RevenueMatrix::AddRawCell(int8_t discount_cent, int64_t quantity_scaled, Q6AccT contribution,
                                 int8_t sign) {
	if (!Q6DiscountInRange(discount_cent) || !Q6QuantityInRange(quantity_scaled)) {
		return;
	}
	AddInPlace(cells[CellIndex(discount_cent, quantity_scaled)], contribution, sign);
}

void Q6RevenueMatrix::AddCell(int8_t discount_cent, int64_t quantity_scaled, Q6AccT contribution, int8_t sign) {
	if (!Q6DiscountInRange(discount_cent) || !Q6QuantityInRange(quantity_scaled)) {
		return;
	}
#if defined(__SIZEOF_INT128__)
	const Q6AccT amount = sign >= 0 ? contribution : -contribution;
#endif
	const auto bucket = static_cast<std::size_t>(Q6RevenueMatrix::QuantityBucket(quantity_scaled));
	for (std::size_t d = static_cast<std::size_t>(discount_cent); d < static_cast<std::size_t>(kQ6DiscountBuckets);
	     ++d) {
		Q6AccT *row = cells.data() + d * static_cast<std::size_t>(kQ6QuantityBuckets);
		for (std::size_t b = bucket; b < static_cast<std::size_t>(kQ6QuantityBuckets); ++b) {
#if defined(__SIZEOF_INT128__)
			row[b] += amount;
#else
			AddInPlace(row[b], contribution, sign);
#endif
		}
	}
}

void Q6RevenueMatrix::PrefixInPlace() {
	constexpr std::size_t kQ = static_cast<std::size_t>(kQ6QuantityBuckets);
	constexpr std::size_t kD = static_cast<std::size_t>(kQ6DiscountBuckets);
	// along quantity (row scan)
	for (std::size_t d = 0; d < kD; ++d) {
		Q6AccT *row = cells.data() + d * kQ;
		for (std::size_t b = 1; b < kQ; ++b)
			AddInPlace(row[b], row[b - 1], 1);
	}
	// along discount: whole rows added (contiguous, vectorizable)
	for (std::size_t d = 1; d < kD; ++d)
		AddQ6Block(cells.data() + d * kQ, cells.data() + (d - 1) * kQ, kQ);
}

void Q6RevenueMatrix::AddMatrix(const Q6RevenueMatrix &other) {
	AddQ6Block(cells.data(), other.cells.data(), cells.size());
}

Q6AccT Q6RevenueMatrix::SumRange(int64_t discount_lower_raw, int64_t discount_upper_raw,
                                 int64_t quantity_upper_raw) const {
	return SumQ6MatrixRange(*this, discount_lower_raw, discount_upper_raw, quantity_upper_raw);
}

void ApplyQ6CellContribution(Q6RevenueMatrix &matrix, int8_t discount_cent, int64_t quantity_scaled,
                             Q6AccT contribution, int8_t sign) {
	matrix.AddCell(discount_cent, quantity_scaled, contribution, sign);
}

Q6RangePlan Q6RangePlan::Make(int64_t discount_lower_raw, int64_t discount_upper_raw,
                              int64_t quantity_upper_raw) {
	Q6RangePlan plan;
	if (quantity_upper_raw <= 0) {
		return plan;
	}
	const auto quantity_limit = std::min<int32_t>(
	    Q6RevenueMatrix::QuantityBucket(quantity_upper_raw - 1), kQ6MaxQuantityBucket);
	const auto discount_lower = std::max<int64_t>(discount_lower_raw, 0);
	const auto discount_upper = std::min<int64_t>(discount_upper_raw, kQ6MaxDiscountCent);
	if (discount_lower > discount_upper || quantity_limit < 0) {
		return plan;
	}
	// Prefix representation: rectangle = P[hi][q] - P[lo - 1][q].
	plan.empty = false;
	plan.hi = static_cast<std::size_t>(discount_upper) * kQ6QuantityBuckets + static_cast<std::size_t>(quantity_limit);
	plan.has_lo = discount_lower > 0;
	if (plan.has_lo) {
		plan.lo = static_cast<std::size_t>(discount_lower - 1) * kQ6QuantityBuckets +
		          static_cast<std::size_t>(quantity_limit);
	}
	return plan;
}

Q6AccT Q6RangePlan::Eval(const Q6RevenueMatrix &matrix) const {
	if (empty) {
		return static_cast<Q6AccT>(0);
	}
	Q6AccT total = matrix.cells[hi];
	if (has_lo) {
		AddInPlace(total, matrix.cells[lo], -1);
	}
	return total;
}

Q6AccT SumQ6MatrixRange(const Q6RevenueMatrix &matrix, int64_t discount_lower_raw,
                        int64_t discount_upper_raw, int64_t quantity_upper_raw) {
	return Q6RangePlan::Make(discount_lower_raw, discount_upper_raw, quantity_upper_raw).Eval(matrix);
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
