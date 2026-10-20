#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "prims/flat_hash_map.hpp"
#include "backends/postgres/api/pg_numeric.hpp"

namespace accelerator_codegen {

using Q1AccT = __int128_t;
using Q1GroupKey = uint16_t;

// Per (shipdate day, group) running sums; raw coefficients at the scales
// 2 / 2 / 2 / 4 / 6 (quantity, price, discount, price*(1-disc), charge).
struct Q1DayGroupStats {
	int64_t count = 0;
	Q1AccT sum_qty = 0;
	Q1AccT sum_base_price = 0;
	Q1AccT sum_disc_price = 0;
	Q1AccT sum_charge = 0;
	Q1AccT sum_discount = 0;
};

// Fenwick (binary indexed) tree over shipdate days, so the `l_shipdate <=
// cutoff` prefix of every group costs O(log D) lookups whatever the number of
// distinct days. The index domain is the whole int32 day range (idx = day -
// INT32_MIN + 1, 1 .. 2^32); only nodes that were ever touched are stored, in
// a copy-on-write sharded hash map keyed (node << 16 | group), so a write
// clones O(log D) shards and a published copy shares the rest.
struct Q1FenwickState {
	aux_state::core::ShardedCowHashMap<Q1DayGroupStats> nodes;
	std::vector<Q1GroupKey> groups; // sorted dictionary of groups ever seen
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

// The contribution of one lineitem (sign +1 insert / -1 delete) to its group.
Q1DayGroupStats Q1RowContribution(int64_t quantity_raw, int64_t extendedprice_raw, int64_t discount_raw,
                                  int64_t tax_raw, int8_t sign);
void AddQ1Stats(Q1DayGroupStats &into, const Q1DayGroupStats &delta);

// Fenwick point update: add `delta` to the (day, group) cell.
void Q1FenwickAdd(Q1FenwickState &state, int32_t day, Q1GroupKey group, const Q1DayGroupStats &delta);

// Groups of the prefix `shipdate <= cutoff_days` (any int64 cutoff: below the
// int32 day range it is empty, above it everything), in group-key order, empty
// groups dropped, finalized to output rows.
std::vector<Q1OutputRow> Q1FenwickPrefixRows(const Q1FenwickState &state, int64_t cutoff_days);

} // namespace accelerator_codegen
