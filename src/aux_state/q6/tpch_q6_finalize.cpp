#include "accelerator_codegen/tpch_q6_finalize.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>

#include "duckdb/common/types/interval.hpp"

namespace accelerator_codegen {
namespace {

#if defined(__SIZEOF_INT128__)
Q6AccT ToNative(int64_t value) {
	return static_cast<Q6AccT>(value);
}

Q6AccT ScaleContribution(int64_t extendedprice_raw, int64_t discount_raw) {
	return ToNative(extendedprice_raw) * ToNative(discount_raw);
}

void AddInPlace(Q6AccT &target, Q6AccT amount, int8_t sign) {
	target += static_cast<Q6AccT>(sign) * amount;
}
#else
Q6AccT ScaleContribution(int64_t extendedprice_raw, int64_t discount_raw) {
	duckdb::hugeint_t price;
	duckdb::hugeint_t discount;
	duckdb::hugeint_t product;
	if (!duckdb::Hugeint::TryConvert(extendedprice_raw, price) || !duckdb::Hugeint::TryConvert(discount_raw, discount) ||
	    !duckdb::Hugeint::TryMultiply(price, discount, product)) {
		return duckdb::Hugeint::Convert(0);
	}
	return product;
}

void AddInPlace(Q6AccT &target, Q6AccT amount, int8_t sign) {
	if (sign >= 0) {
		duckdb::Hugeint::AddInPlace(target, amount);
	} else {
		if (!duckdb::Hugeint::TrySubtractInPlace(target, amount)) {
			throw duckdb::OutOfRangeException("Overflow while updating Q6 revenue");
		}
	}
}
#endif

void EnsurePrefixCapacity(std::vector<Q6AccT> &prefix, size_t min_size) {
	if (prefix.size() >= min_size) {
		return;
	}
	const auto carry = prefix.empty() ? static_cast<Q6AccT>(0) : prefix.back();
	prefix.resize(min_size, carry);
}

} // namespace

Q6AccT Q6RowContributionRaw(int64_t extendedprice_raw, int64_t discount_raw) {
	return ScaleContribution(extendedprice_raw, discount_raw);
}

void ApplyQ6RowContribution(Q6DayPartition &partition, int8_t discount_cent, int64_t quantity_scaled,
                            Q6AccT contribution, int8_t sign) {
	if (quantity_scaled < 0) {
		return;
	}
	auto &prefix = partition.discount_prefix[discount_cent];
	const auto index = static_cast<size_t>(quantity_scaled);
	EnsurePrefixCapacity(prefix, index + 1);
	for (size_t k = index; k < prefix.size(); k++) {
		AddInPlace(prefix[k], contribution, sign);
	}
}

void ApplyQ6PrefixContributions(Q6DayPartition &partition, const std::vector<Q6PrefixContribution> &contributions) {
	std::unordered_map<int8_t, std::unordered_map<size_t, Q6AccT>> net_at_index;
	for (const auto &row : contributions) {
		if (row.quantity_scaled < 0) {
			continue;
		}
		const auto index = static_cast<size_t>(row.quantity_scaled);
		net_at_index[row.discount_cent][index] += static_cast<Q6AccT>(row.sign) * row.contribution;
	}
	for (auto &cent_entry : net_at_index) {
		auto &index_nets = cent_entry.second;
		std::vector<std::pair<size_t, Q6AccT>> sorted_deltas;
		sorted_deltas.reserve(index_nets.size());
		size_t max_index = 0;
		size_t min_index = std::numeric_limits<size_t>::max();
		for (const auto &index_entry : index_nets) {
			if (index_entry.second == static_cast<Q6AccT>(0)) {
				continue;
			}
			sorted_deltas.emplace_back(index_entry.first, index_entry.second);
			max_index = std::max(max_index, index_entry.first);
			min_index = std::min(min_index, index_entry.first);
		}
		if (sorted_deltas.empty()) {
			continue;
		}
		std::sort(sorted_deltas.begin(), sorted_deltas.end(),
		          [](const auto &left, const auto &right) { return left.first < right.first; });
		auto &prefix = partition.discount_prefix[cent_entry.first];
		EnsurePrefixCapacity(prefix, max_index + 1);
		Q6AccT running = static_cast<Q6AccT>(0);
		size_t delta_idx = 0;
		for (size_t k = 0; k < prefix.size(); k++) {
			while (delta_idx < sorted_deltas.size() && sorted_deltas[delta_idx].first == k) {
				running += sorted_deltas[delta_idx].second;
				delta_idx++;
			}
			if (k >= min_index && running != static_cast<Q6AccT>(0)) {
				AddInPlace(prefix[k], running, 1);
			}
		}
	}
}

Q6AccT LookupQ6PrefixRevenue(const Q6DayPartition &partition, int8_t discount_cent, int64_t quantity_upper_raw) {
	const auto it = partition.discount_prefix.find(discount_cent);
	if (it == partition.discount_prefix.end() || it->second.empty()) {
		return static_cast<Q6AccT>(0);
	}
	const auto &prefix = it->second;
	if (quantity_upper_raw <= 0) {
		return static_cast<Q6AccT>(0);
	}
	const auto index = static_cast<size_t>(quantity_upper_raw - 1);
	if (index >= prefix.size()) {
		return prefix.back();
	}
	return prefix[index];
}

void MergeQ6DayRevenue(Q6AccT &accumulator, const Q6DayPartition &partition, int64_t discount_lower_raw,
                       int64_t discount_upper_raw, int64_t quantity_upper_raw) {
	for (int64_t cent = discount_lower_raw; cent <= discount_upper_raw; ++cent) {
		AddInPlace(accumulator, LookupQ6PrefixRevenue(partition, static_cast<int8_t>(cent), quantity_upper_raw), 1);
	}
}

duckdb::hugeint_t Q6RevenueFromAccumulator(Q6AccT raw) {
#if defined(__SIZEOF_INT128__)
	return duckdb::hugeint_t(static_cast<int64_t>(raw >> 64), static_cast<uint64_t>(raw));
#else
	return raw;
#endif
}

duckdb::date_t Q6ComputeExclusiveEndDate(duckdb::date_t start_date) {
	duckdb::interval_t one_year;
	one_year.months = duckdb::Interval::MONTHS_PER_YEAR;
	one_year.days = 0;
	one_year.micros = 0;
	return duckdb::Interval::Add(start_date, one_year);
}

} // namespace accelerator_codegen
