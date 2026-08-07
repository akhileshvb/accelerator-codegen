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
// discount in [0.00, 0.10] and quantity in [0, 50.00].
constexpr int32_t kQ6DiscountBuckets = 11;
constexpr int32_t kQ6QuantityBuckets = 5001;
constexpr int32_t kQ6MaxDiscountCent = kQ6DiscountBuckets - 1;
constexpr int32_t kQ6MaxQuantityScaled = kQ6QuantityBuckets - 1;

struct Q6RevenueMatrix {
	std::array<Q6AccT, static_cast<std::size_t>(kQ6DiscountBuckets * kQ6QuantityBuckets)> cells{};

	static constexpr std::size_t CellIndex(int8_t discount_cent, int64_t quantity_scaled) {
		return static_cast<std::size_t>(discount_cent) * static_cast<std::size_t>(kQ6QuantityBuckets) +
		       static_cast<std::size_t>(quantity_scaled);
	}

	Q6AccT &At(int8_t discount_cent, int64_t quantity_scaled) {
		return cells[CellIndex(discount_cent, quantity_scaled)];
	}

	const Q6AccT &At(int8_t discount_cent, int64_t quantity_scaled) const {
		return cells[CellIndex(discount_cent, quantity_scaled)];
	}

	void AddCell(int8_t discount_cent, int64_t quantity_scaled, Q6AccT contribution, int8_t sign = 1);

	Q6AccT SumRange(int64_t discount_lower_raw, int64_t discount_upper_raw,
	                int64_t quantity_upper_raw) const;
};

Q6AccT Q6RowContributionRaw(int64_t extendedprice_raw, int64_t discount_raw);

void ApplyQ6CellContribution(Q6RevenueMatrix &matrix, int8_t discount_cent, int64_t quantity_scaled,
                             Q6AccT contribution, int8_t sign = 1);

Q6AccT SumQ6MatrixRange(const Q6RevenueMatrix &matrix, int64_t discount_lower_raw,
                        int64_t discount_upper_raw, int64_t quantity_upper_raw);

duckdb::hugeint_t Q6RevenueFromAccumulator(Q6AccT raw);

duckdb::date_t Q6ComputeExclusiveEndDate(duckdb::date_t start_date);

} // namespace accelerator_codegen
