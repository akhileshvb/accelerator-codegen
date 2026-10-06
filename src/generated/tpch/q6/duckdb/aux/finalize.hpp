#pragma once

#include <array>
#include <cstdint>

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"

namespace accelerator_codegen {

#if defined(__SIZEOF_INT128__)
using Q6AccT = __int128_t;
#else
using Q6AccT = duckdb::hugeint_t;
#endif

constexpr uint8_t kQ6RevenueDecimalWidth = 38;
constexpr uint8_t kQ6RevenueDecimalScale = 4;

// Discount and quantity are stored at cent precision (scale 2). TPC-H bounds are
// discount in [0.00, 0.10] and integer quantity in [0, 50]. Cells are indexed by
// integer quantity (quantity_raw / 100), not the 0.01 grid.
constexpr int32_t kQ6DiscountBuckets = 11;
constexpr int32_t kQ6QuantityScale = 100;
constexpr int32_t kQ6QuantityBuckets = 51;
constexpr int32_t kQ6MaxDiscountCent = kQ6DiscountBuckets - 1;
constexpr int32_t kQ6MaxQuantityBucket = kQ6QuantityBuckets - 1;

// The matrix stores an inclusive 2D prefix sum over (discount cent, integer
// quantity bucket):  cells[d][b] = sum of the raw cells (d' <= d, b' <= b).
// SumRange is then at most two loads (one row difference) instead of a
// rectangle walk, and the matrix is a flat, 64-byte aligned block of 128-bit
// accumulators. Updates add into the suffix rectangle; batches of updates are
// accumulated in a raw (non-prefix) matrix, prefix-transformed once and added.
struct alignas(64) Q6RevenueMatrix {
	std::array<Q6AccT, static_cast<std::size_t>(kQ6DiscountBuckets * kQ6QuantityBuckets)> cells {};

	static constexpr int32_t QuantityBucket(int64_t quantity_scaled) {
		if (quantity_scaled < 0)
			return -1;
		return static_cast<int32_t>(quantity_scaled / kQ6QuantityScale);
	}

	static constexpr std::size_t CellIndex(int8_t discount_cent, int64_t quantity_scaled) {
		return static_cast<std::size_t>(discount_cent) * static_cast<std::size_t>(kQ6QuantityBuckets) +
		       static_cast<std::size_t>(QuantityBucket(quantity_scaled));
	}

	// Prefix-representation update: adds sign * contribution to the raw cell
	// (discount_cent, quantity bucket), i.e. to every prefix cell it dominates.
	// Out-of-range cells are ignored.
	void AddCell(int8_t discount_cent, int64_t quantity_scaled, Q6AccT contribution, int8_t sign = 1);

	// Raw (non-prefix) accumulation, used to build a matrix or a delta before
	// PrefixInPlace(). Out-of-range cells are ignored.
	void AddRawCell(int8_t discount_cent, int64_t quantity_scaled, Q6AccT contribution, int8_t sign = 1);
	// Raw cells -> inclusive 2D prefix sums.
	void PrefixInPlace();
	// Element-wise add of another matrix in prefix representation.
	void AddMatrix(const Q6RevenueMatrix &other);

	Q6AccT SumRange(int64_t discount_lower_raw, int64_t discount_upper_raw, int64_t quantity_upper_raw) const;
};

// The cells a SumRange(discount range, quantity limit) reads, resolved once per
// query: Eval is two loads and a subtract per matrix.
struct Q6RangePlan {
	bool empty = true;
	bool has_lo = false;
	std::size_t hi = 0;
	std::size_t lo = 0;

	static Q6RangePlan Make(int64_t discount_lower_raw, int64_t discount_upper_raw,
	                        int64_t quantity_upper_raw);
	Q6AccT Eval(const Q6RevenueMatrix &matrix) const;
	void Prefetch(const Q6RevenueMatrix &matrix) const {
		__builtin_prefetch(&matrix.cells[hi]);
		if (has_lo)
			__builtin_prefetch(&matrix.cells[lo]);
	}
};

Q6AccT Q6RowContributionRaw(int64_t extendedprice_raw, int64_t discount_raw);

void ApplyQ6CellContribution(Q6RevenueMatrix &matrix, int8_t discount_cent, int64_t quantity_scaled,
                             Q6AccT contribution, int8_t sign = 1);

Q6AccT SumQ6MatrixRange(const Q6RevenueMatrix &matrix, int64_t discount_lower_raw,
                        int64_t discount_upper_raw, int64_t quantity_upper_raw);

duckdb::hugeint_t Q6RevenueFromAccumulator(Q6AccT raw);

duckdb::date_t Q6ComputeExclusiveEndDate(duckdb::date_t start_date);

} // namespace accelerator_codegen
