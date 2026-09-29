#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/stateful/core/pg_numeric.hpp"
#include "duckdb/common/types/hugeint.hpp"

namespace accelerator_codegen {

#if defined(__SIZEOF_INT128__)
using Q1AccT = __int128_t;
#else
using Q1AccT = int64_t;
#endif

using Q1GroupKey = uint16_t;

struct Q1DayGroupStats {
	int64_t count = 0;
	int64_t sum_qty = 0;
	int64_t sum_base_price = 0;
	Q1AccT sum_disc_price = 0;
	Q1AccT sum_charge = 0;
	Q1AccT sum_discount = 0;
};

struct Q1DayPartition {
	std::unordered_map<Q1GroupKey, Q1DayGroupStats> groups;
};

struct Q1MergedGroupStats {
	int64_t count = 0;
	Q1AccT sum_qty = 0;
	Q1AccT sum_base_price = 0;
	Q1AccT sum_disc_price = 0;
	Q1AccT sum_charge = 0;
	Q1AccT sum_discount = 0;
};

// Exact PostgreSQL NUMERIC results: sums are integer coefficients at scales
// 2 / 2 / 4 / 6; averages are avg(numeric) of the scale-2 sums
// (pg_numeric::AvgOfScaledSum: PostgreSQL result scale, half away from zero).
struct Q1OutputRow {
	Q1GroupKey group_key = 0;
	Q1AccT sum_qty = 0;
	Q1AccT sum_base_price = 0;
	Q1AccT sum_disc_price = 0;
	Q1AccT sum_charge = 0;
	pg_numeric::Decimal avg_qty {};
	pg_numeric::Decimal avg_price {};
	pg_numeric::Decimal avg_disc {};
	int64_t count_order = 0;
};

Q1GroupKey PackQ1GroupKey(char returnflag, char linestatus);
std::string UnpackQ1Returnflag(Q1GroupKey key);
std::string UnpackQ1Linestatus(Q1GroupKey key);

void ApplyQ1RowContribution(Q1DayGroupStats &stats, int64_t quantity_raw, int64_t extendedprice_raw,
                            int64_t discount_raw, int64_t tax_raw, int8_t sign);

void MergeQ1DayGroupStats(Q1MergedGroupStats &merged, const Q1DayGroupStats &day_stats);

std::vector<Q1OutputRow> FinalizeQ1MergedGroups(const std::unordered_map<Q1GroupKey, Q1MergedGroupStats> &merged);

} // namespace accelerator_codegen
