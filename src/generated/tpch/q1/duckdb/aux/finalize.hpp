#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

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

struct Q1OutputRow {
	Q1GroupKey group_key = 0;
	duckdb::hugeint_t sum_qty {};
	duckdb::hugeint_t sum_base_price {};
	duckdb::hugeint_t sum_disc_price {};
	duckdb::hugeint_t sum_charge {};
	double avg_qty = 0.0;
	double avg_price = 0.0;
	double avg_disc = 0.0;
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
