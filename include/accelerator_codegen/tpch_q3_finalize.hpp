#pragma once

#include <algorithm>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"

namespace accelerator_codegen {

#if defined(__SIZEOF_INT128__)
using Q3AccT = __int128_t;
#else
using Q3AccT = duckdb::hugeint_t;
#endif

constexpr uint8_t kQ3RevenueDecimalWidth = 38;
constexpr uint8_t kQ3RevenueDecimalScale = 4;
constexpr duckdb::idx_t kQ3TopK = 10;

struct Q3OrderState {
	duckdb::date_t o_orderdate {};
	int32_t o_shippriority = 0;
	int64_t o_custkey = 0;
	std::unordered_map<int32_t, Q3AccT> shipdate_buckets;
};

struct Q3SegmentPartition {
	std::unordered_map<int64_t, Q3OrderState> orders;
};

struct Q3OutputRow {
	int64_t orderkey = 0;
	duckdb::hugeint_t revenue {};
	duckdb::date_t orderdate {};
	int32_t shippriority = 0;
};

Q3AccT DiscountedRevenueRaw(int64_t extendedprice_raw, int64_t discount_raw);
void AddBucketRevenue(Q3AccT &bucket, int64_t extendedprice_raw, int64_t discount_raw, int8_t sign = 1);
void AddAccumulatedRevenue(Q3AccT &target, const Q3AccT &amount, int8_t sign = 1);

duckdb::hugeint_t Q3RevenueFromAccumulator(Q3AccT raw);

Q3AccT SumBucketsAboveCutoff(const Q3OrderState &order, duckdb::date_t cutoff_date);

bool BetterQ3Row(const Q3OutputRow &lhs, const Q3OutputRow &rhs);

std::vector<Q3OutputRow> ComposeQ3TopK(const Q3SegmentPartition &partition, duckdb::date_t cutoff_date);

} // namespace accelerator_codegen
