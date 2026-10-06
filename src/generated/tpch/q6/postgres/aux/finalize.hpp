#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/stateful/core/cow_fenwick_g1.hpp"

namespace accelerator_codegen {

using Q6AccT = __int128_t;

// Distinct observed values of one lineitem column (raw coefficients at scale
// 2), interned in first-seen order; the id is stable, so growing the
// dictionary never renumbers (and never rewrites) existing state.
struct Q6ValueDict {
	std::vector<int64_t> values; // id -> raw value
	std::unordered_map<int64_t, uint32_t> ids;

	uint32_t Intern(int64_t raw);
	std::size_t size() const {
		return values.size();
	}
};

// One Fenwick node: revenue sum and row count per (discount id, quantity id)
// cell, row-major with the node's own grid dimensions (ids beyond them read
// as empty; the grid is regrown, ids kept, when a write meets a larger
// dictionary).
struct Q6Node {
	uint32_t discounts = 0;
	uint32_t quantities = 0;
	std::vector<Q6AccT> revenue;
	std::vector<int64_t> count;
};

// The whole state (one image, SF-independent): a copy-on-write Fenwick tree
// over shipdate days whose nodes are discount x quantity grids, plus the two
// value dictionaries. Revenue is sum(extendedprice * discount) at scale 4.
struct Q6State {
	aux_state::core::CowFenwick<Q6Node> days;
	Q6ValueDict discounts;
	Q6ValueDict quantities;
};

// Add `revenue` / `count` rows to the (day, discount, quantity) cell.
void Q6AddCell(Q6State &state, int32_t day, int64_t discount_raw, int64_t quantity_raw, Q6AccT revenue,
               int64_t count);

// The row's SUM(l_extendedprice * l_discount) term (scale 4).
Q6AccT Q6RowRevenue(int64_t extendedprice_raw, int64_t discount_raw);

// A resolved query: shipdate in [start_days, end_days), discount raw (cents)
// in [discount_lo, discount_hi], quantity raw (hundredths) <= quantity_hi.
struct Q6Predicate {
	int64_t start_days = 0;
	int64_t end_days = 0;
	int64_t discount_lo = 0;
	int64_t discount_hi = 0;
	int64_t quantity_hi = 0;
};

// Resolve the template parameters exactly. The double arguments are read as
// the shortest decimal that round-trips them (the literal the caller wrote);
// the band is [d - 0.01, d + 0.01] and the quantity bound `< q`, evaluated in
// exact decimal arithmetic against cent-precision columns. False: an argument
// is NaN (no row can match).
// A template argument as floor(100 * v) and whether 100 * v is an integer.
struct Q6Scaled100 {
	__int128_t floor100 = 0;
	bool exact = true;
};
// From a double (false: NaN) or from an exact decimal coefficient / scale.
bool Q6Scaled100FromDouble(double v, Q6Scaled100 &out);
Q6Scaled100 Q6Scaled100FromDecimal(int64_t raw, int32_t scale);

bool Q6ResolvePredicate(int64_t start_days, const Q6Scaled100 &discount, const Q6Scaled100 &quantity,
                        Q6Predicate &out);

struct Q6Result {
	Q6AccT revenue = 0;
	int64_t count = 0; // qualifying rows (SUM over none is NULL)
};

Q6Result Q6Query(const Q6State &state, const Q6Predicate &predicate);

} // namespace accelerator_codegen
