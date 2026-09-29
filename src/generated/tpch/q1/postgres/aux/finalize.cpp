#include "tpch/q1/postgres/aux/finalize.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/value.hpp"

namespace accelerator_codegen {
namespace {

constexpr int kDecimalScale2 = 2;

inline void AddAcc(Q1AccT &accumulator, Q1AccT value) {
	accumulator += value;
}

inline void AddAcc(Q1AccT &accumulator, int64_t value) {
	accumulator += static_cast<Q1AccT>(value);
}

} // namespace

Q1GroupKey PackQ1GroupKey(char returnflag, char linestatus) {
	return static_cast<Q1GroupKey>((static_cast<uint16_t>(static_cast<uint8_t>(returnflag)) << 8) |
	                                static_cast<uint8_t>(linestatus));
}

std::string UnpackQ1Returnflag(Q1GroupKey key) {
	return std::string(1, static_cast<char>(key >> 8));
}

std::string UnpackQ1Linestatus(Q1GroupKey key) {
	return std::string(1, static_cast<char>(key & 0xFF));
}

void ApplyQ1RowContribution(Q1DayGroupStats &stats, int64_t quantity_raw, int64_t extendedprice_raw,
                            int64_t discount_raw, int64_t tax_raw, int8_t sign) {
	stats.count += sign;
	stats.sum_qty += sign * quantity_raw;
	stats.sum_base_price += sign * extendedprice_raw;
	stats.sum_discount += sign * discount_raw;
	const auto one_minus_discount_raw = static_cast<int64_t>(100 - discount_raw);
	const Q1AccT disc_price_raw = static_cast<Q1AccT>(extendedprice_raw) * static_cast<Q1AccT>(one_minus_discount_raw);
	stats.sum_disc_price += static_cast<Q1AccT>(sign) * disc_price_raw;
	const auto one_plus_tax_raw = static_cast<int64_t>(100 + tax_raw);
	stats.sum_charge += static_cast<Q1AccT>(sign) * disc_price_raw * static_cast<Q1AccT>(one_plus_tax_raw);
}

void MergeQ1DayGroupStats(Q1MergedGroupStats &merged, const Q1DayGroupStats &day_stats) {
	merged.count += day_stats.count;
	AddAcc(merged.sum_qty, day_stats.sum_qty);
	AddAcc(merged.sum_base_price, day_stats.sum_base_price);
	AddAcc(merged.sum_disc_price, day_stats.sum_disc_price);
	AddAcc(merged.sum_charge, day_stats.sum_charge);
	AddAcc(merged.sum_discount, day_stats.sum_discount);
}

std::vector<Q1OutputRow> FinalizeQ1MergedGroups(const std::unordered_map<Q1GroupKey, Q1MergedGroupStats> &merged) {
	std::vector<Q1OutputRow> rows;
	rows.reserve(merged.size());
	for (const auto &entry : merged) {
		const auto &stats = entry.second;
		if (stats.count <= 0) {
			continue;
		}
		Q1OutputRow row;
		row.group_key = entry.first;
		row.sum_qty = stats.sum_qty;
		row.sum_base_price = stats.sum_base_price;
		row.sum_disc_price = stats.sum_disc_price;
		row.sum_charge = stats.sum_charge;
		row.avg_qty = pg_numeric::AvgOfScaledSum(stats.sum_qty, kDecimalScale2, stats.count);
		row.avg_price = pg_numeric::AvgOfScaledSum(stats.sum_base_price, kDecimalScale2, stats.count);
		row.avg_disc = pg_numeric::AvgOfScaledSum(stats.sum_discount, kDecimalScale2, stats.count);
		row.count_order = stats.count;
		rows.push_back(row);
	}
	std::sort(rows.begin(), rows.end(),
	          [](const Q1OutputRow &lhs, const Q1OutputRow &rhs) { return lhs.group_key < rhs.group_key; });
	return rows;
}

} // namespace accelerator_codegen
