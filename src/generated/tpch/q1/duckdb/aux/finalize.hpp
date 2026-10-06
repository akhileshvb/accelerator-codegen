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

// (returnflag, linestatus) has at most 3 x 2 combinations in TPC-H; 8 slots
// are kept in a flat, trivially copyable block (no maps, no allocation).
constexpr uint32_t kQ1MaxGroups = 8;

struct Q1DayPartition {
	uint32_t n = 0;
	Q1GroupKey keys[kQ1MaxGroups] = {};
	Q1DayGroupStats stats[kQ1MaxGroups];

	int Find(Q1GroupKey key) const {
		int found = -1;
		for (uint32_t i = 0; i < n; ++i)
			found = keys[i] == key ? static_cast<int>(i) : found;
		return found;
	}
	// nullptr when all slots are taken by other groups.
	Q1DayGroupStats *FindOrAdd(Q1GroupKey key) {
		const int at = Find(key);
		if (at >= 0)
			return &stats[at];
		if (n == kQ1MaxGroups)
			return nullptr;
		keys[n] = key;
		stats[n] = Q1DayGroupStats {};
		return &stats[n++];
	}
	void EraseAt(uint32_t at) {
		--n;
		keys[at] = keys[n];
		stats[at] = stats[n];
	}
	bool Empty() const {
		return n == 0;
	}
};

struct Q1MergedGroupStats {
	int64_t count = 0;
	Q1AccT sum_qty = 0;
	Q1AccT sum_base_price = 0;
	Q1AccT sum_disc_price = 0;
	Q1AccT sum_charge = 0;
	Q1AccT sum_discount = 0;
};

// Running merge of dyadic nodes for one read.
struct Q1MergedFlat {
	uint32_t n = 0;
	Q1GroupKey keys[kQ1MaxGroups] = {};
	Q1MergedGroupStats stats[kQ1MaxGroups];

	// Adds a whole node; false when more distinct groups than slots.
	bool AddPartition(const Q1DayPartition &node);
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

// Allocation-free: fills `rows` (capacity kQ1MaxGroups) sorted by group key,
// returns the number of rows. Same arithmetic as the map overload.
uint32_t FinalizeQ1MergedFlat(const Q1MergedFlat &merged, Q1OutputRow *rows);

} // namespace accelerator_codegen
