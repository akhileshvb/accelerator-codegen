#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

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

struct Q6DayPartition {
	std::unordered_map<int8_t, std::vector<Q6AccT>> discount_prefix;
};

Q6AccT Q6RowContributionRaw(int64_t extendedprice_raw, int64_t discount_raw);
struct Q6PrefixContribution {
	int8_t discount_cent = 0;
	int64_t quantity_scaled = 0;
	Q6AccT contribution = static_cast<Q6AccT>(0);
	int8_t sign = 1;
};

void ApplyQ6RowContribution(Q6DayPartition &partition, int8_t discount_cent, int64_t quantity_scaled,
                            Q6AccT contribution, int8_t sign = 1);
void ApplyQ6PrefixContributions(Q6DayPartition &partition, const std::vector<Q6PrefixContribution> &contributions);
Q6AccT LookupQ6PrefixRevenue(const Q6DayPartition &partition, int8_t discount_cent, int64_t quantity_upper_raw);
void MergeQ6DayRevenue(Q6AccT &accumulator, const Q6DayPartition &partition, int64_t discount_lower_raw,
                       int64_t discount_upper_raw, int64_t quantity_upper_raw);
duckdb::hugeint_t Q6RevenueFromAccumulator(Q6AccT raw);

duckdb::date_t Q6ComputeExclusiveEndDate(duckdb::date_t start_date);

} // namespace accelerator_codegen
