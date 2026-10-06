#include "tpch/q6/mysql/aux/finalize.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "accelerator_codegen/stateful/core/sql_date.hpp"

namespace accelerator_codegen {
namespace {

using Int128 = __int128_t;

int64_t Saturate(Int128 value) {
	if (value > INT64_MAX)
		return INT64_MAX;
	if (value < INT64_MIN)
		return INT64_MIN;
	return static_cast<int64_t>(value);
}

// floor(100 * v) and whether 100 * v is an integer, where v is the shortest
// decimal that round-trips the double (what `0.065` or `24` in a SQL literal
// became). Magnitudes beyond 1e30 saturate (no cent-precision column reaches).
bool Floor100(double v, Int128 &floor100, bool &exact) {
	if (std::isnan(v))
		return false;
	if (std::isinf(v)) {
		floor100 = v > 0 ? Int128(1) * 1000000000000000000LL * 1000000000000LL
		                 : -Int128(1) * 1000000000000000000LL * 1000000000000LL;
		exact = true;
		return true;
	}
	if (v == 0) {
		floor100 = 0;
		exact = true;
		return true;
	}
	char buf[40];
	int precision = 0;
	for (; precision < 17; ++precision) {
		std::snprintf(buf, sizeof(buf), "%.*e", precision, v);
		if (std::strtod(buf, nullptr) == v)
			break;
	}
	// buf = [-]d[.ddd]e[+-]XX: mantissa digits D, value = D * 10^(E - precision).
	const bool negative = buf[0] == '-';
	Int128 digits = 0;
	const char *p = buf + (negative ? 1 : 0);
	for (; *p && *p != 'e'; ++p)
		if (*p != '.')
			digits = digits * 10 + (*p - '0');
	const int exp10 = std::atoi(p + 1) - precision;
	const int shift = exp10 + 2; // 100 * v = digits * 10^shift
	Int128 magnitude = 0;
	bool is_exact = true;
	if (shift >= 0) {
		if (shift > 20) {
			magnitude = Int128(1) * 1000000000000000000LL * 1000000000000LL;
		} else {
			magnitude = digits;
			for (int i = 0; i < shift; ++i)
				magnitude *= 10;
		}
	} else if (-shift > 18) {
		magnitude = 0; // digits < 10^18: the integer part is 0, the fraction is not
		is_exact = false;
	} else {
		Int128 divisor = 1;
		for (int i = 0; i < -shift; ++i)
			divisor *= 10;
		magnitude = digits / divisor;
		is_exact = digits % divisor == 0;
	}
	exact = is_exact;
	floor100 = negative ? (is_exact ? -magnitude : -magnitude - 1) : magnitude;
	return true;
}

} // namespace

uint32_t Q6ValueDict::Intern(int64_t raw) {
	const auto it = ids.find(raw);
	if (it != ids.end())
		return it->second;
	const auto id = static_cast<uint32_t>(values.size());
	values.push_back(raw);
	ids.emplace(raw, id);
	return id;
}

Q6AccT Q6RowRevenue(int64_t extendedprice_raw, int64_t discount_raw) {
	return static_cast<Q6AccT>(extendedprice_raw) * static_cast<Q6AccT>(discount_raw);
}

void Q6AddCell(Q6State &state, int32_t day, int64_t discount_raw, int64_t quantity_raw, Q6AccT revenue,
               int64_t count) {
	const uint32_t d = state.discounts.Intern(discount_raw);
	const uint32_t q = state.quantities.Intern(quantity_raw);
	const auto nd = static_cast<uint32_t>(state.discounts.size());
	const auto nq = static_cast<uint32_t>(state.quantities.size());
	state.days.Update(day, [&](Q6Node &node) {
		if (node.discounts < nd || node.quantities < nq) {
			// Regrow to the current dictionaries (ids stay, cells move).
			std::vector<Q6AccT> revenue_grid(static_cast<std::size_t>(nd) * nq, 0);
			std::vector<int64_t> count_grid(static_cast<std::size_t>(nd) * nq, 0);
			for (uint32_t i = 0; i < node.discounts; ++i)
				for (uint32_t j = 0; j < node.quantities; ++j) {
					revenue_grid[static_cast<std::size_t>(i) * nq + j] =
					    node.revenue[static_cast<std::size_t>(i) * node.quantities + j];
					count_grid[static_cast<std::size_t>(i) * nq + j] =
					    node.count[static_cast<std::size_t>(i) * node.quantities + j];
				}
			node.revenue = std::move(revenue_grid);
			node.count = std::move(count_grid);
			node.discounts = nd;
			node.quantities = nq;
		}
		const auto cell = static_cast<std::size_t>(d) * node.quantities + q;
		node.revenue[cell] += revenue;
		node.count[cell] += count;
	});
}

bool Q6ResolvePredicate(int64_t start_days, double discount, double quantity, Q6Predicate &out) {
	Int128 fl = 0;
	bool exact = false;
	if (!Floor100(discount, fl, exact))
		return false;
	// raw/100 >= d - 0.01  <=>  raw >= ceil(100d) - 1;  raw/100 <= d + 0.01  <=>  raw <= floor(100d) + 1.
	out.discount_lo = Saturate((exact ? fl : fl + 1) - 1);
	out.discount_hi = Saturate(fl + 1);
	if (!Floor100(quantity, fl, exact))
		return false;
	// raw/100 < q  <=>  raw <= ceil(100q) - 1.
	out.quantity_hi = Saturate((exact ? fl : fl + 1) - 1);
	out.start_days = start_days;
	// shipdate < start + INTERVAL '1' YEAR (end-of-month clamped).
	out.end_days = aux_state::core::sql_date::AddYears(start_days, 1);
	return true;
}

Q6Result Q6Query(const Q6State &state, const Q6Predicate &predicate) {
	Q6Result result;
	if (predicate.start_days >= predicate.end_days)
		return result;
	std::vector<uint32_t> discount_ids, quantity_ids;
	for (uint32_t i = 0; i < state.discounts.size(); ++i) {
		const auto v = state.discounts.values[i];
		if (v >= predicate.discount_lo && v <= predicate.discount_hi)
			discount_ids.push_back(i);
	}
	for (uint32_t j = 0; j < state.quantities.size(); ++j)
		if (state.quantities.values[j] <= predicate.quantity_hi)
			quantity_ids.push_back(j);
	if (discount_ids.empty() || quantity_ids.empty())
		return result;

	const auto accumulate = [&](int sign) {
		return [&, sign](const Q6Node &node) {
			for (const auto d : discount_ids) {
				if (d >= node.discounts)
					break; // ids ascend; the rest are beyond this node's grid
				const auto row = static_cast<std::size_t>(d) * node.quantities;
				for (const auto q : quantity_ids) {
					if (q >= node.quantities)
						break;
					result.revenue += sign * node.revenue[row + q];
					result.count += sign * node.count[row + q];
				}
			}
		};
	};
	// Days [start, end) = prefix(end - 1) - prefix(start - 1).
	state.days.VisitPrefix(predicate.end_days - 1, accumulate(1));
	state.days.VisitPrefix(predicate.start_days - 1, accumulate(-1));
	return result;
}

} // namespace accelerator_codegen
