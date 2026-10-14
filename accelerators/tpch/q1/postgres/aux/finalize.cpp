#include "tpch/q1/postgres/aux/finalize.hpp"

#include <algorithm>

namespace accelerator_codegen {
namespace {

constexpr int kDecimalScale2 = 2;
constexpr int64_t kFenwickDomain = int64_t{1} << 32; // day indexes 1 .. 2^32

inline int64_t NodeKey(int64_t node, Q1GroupKey group) {
	return (node << 16) | group;
}

inline int64_t DayIndex(int64_t day) {
	return day - static_cast<int64_t>(INT32_MIN) + 1;
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

Q1DayGroupStats Q1RowContribution(int64_t quantity_raw, int64_t extendedprice_raw, int64_t discount_raw,
                                  int64_t tax_raw, int8_t sign) {
	Q1DayGroupStats stats;
	const Q1AccT s = sign;
	const Q1AccT disc_price = static_cast<Q1AccT>(extendedprice_raw) * static_cast<Q1AccT>(100 - discount_raw);
	stats.count = sign;
	stats.sum_qty = s * quantity_raw;
	stats.sum_base_price = s * extendedprice_raw;
	stats.sum_discount = s * discount_raw;
	stats.sum_disc_price = s * disc_price;
	stats.sum_charge = s * disc_price * static_cast<Q1AccT>(100 + tax_raw);
	return stats;
}

void AddQ1Stats(Q1DayGroupStats &into, const Q1DayGroupStats &delta) {
	into.count += delta.count;
	into.sum_qty += delta.sum_qty;
	into.sum_base_price += delta.sum_base_price;
	into.sum_disc_price += delta.sum_disc_price;
	into.sum_charge += delta.sum_charge;
	into.sum_discount += delta.sum_discount;
}

void Q1FenwickAdd(Q1FenwickState &state, int32_t day, Q1GroupKey group, const Q1DayGroupStats &delta) {
	const auto it = std::lower_bound(state.groups.begin(), state.groups.end(), group);
	if (it == state.groups.end() || *it != group)
		state.groups.insert(it, group);
	for (int64_t node = DayIndex(day); node <= kFenwickDomain; node += node & -node)
		AddQ1Stats(state.nodes[NodeKey(node, group)], delta);
}

std::vector<Q1OutputRow> Q1FenwickPrefixRows(const Q1FenwickState &state, int64_t cutoff_days) {
	std::vector<Q1OutputRow> rows;
	if (cutoff_days < INT32_MIN)
		return rows;
	const int64_t last = std::min<int64_t>(DayIndex(std::min<int64_t>(cutoff_days, INT32_MAX)), kFenwickDomain);
	for (const auto group : state.groups) { // sorted: output order
		Q1MergedGroupStats stats;
		for (int64_t node = last; node > 0; node -= node & -node) {
			const auto *cell = state.nodes.Find(NodeKey(node, group));
			if (!cell)
				continue;
			stats.count += cell->count;
			stats.sum_qty += cell->sum_qty;
			stats.sum_base_price += cell->sum_base_price;
			stats.sum_disc_price += cell->sum_disc_price;
			stats.sum_charge += cell->sum_charge;
			stats.sum_discount += cell->sum_discount;
		}
		if (stats.count <= 0)
			continue;
		Q1OutputRow row;
		row.group_key = group;
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
	return rows;
}

} // namespace accelerator_codegen
