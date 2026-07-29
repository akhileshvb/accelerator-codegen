#include "accelerator_codegen/tpch_q10_finalize.hpp"

#include <algorithm>
#include <cstring>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/interval.hpp"

namespace accelerator_codegen {
namespace {

#if defined(__SIZEOF_INT128__)
duckdb::hugeint_t FromAccumulator(Q10AccT value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}
#else
duckdb::hugeint_t FromAccumulator(const Q10AccT &value) {
	return value;
}
#endif

int32_t ExtractYear(int32_t partition_key) {
	return partition_key / 100;
}

int32_t ExtractMonth(int32_t partition_key) {
	return partition_key % 100;
}

} // namespace

void Q10SideStores::ClearCustomers() {
	customer_active.clear();
	customer_name.clear();
	customer_acctbal_raw.clear();
	customer_nationkey.clear();
	customer_address.clear();
	customer_phone.clear();
	customer_comment.clear();
}

void Q10SideStores::ReserveCustomers() {
	const auto capacity = static_cast<size_t>(kQ10TpchMaxCustkey) + 1;
	customer_active.assign(capacity, 0);
	customer_name.resize(capacity);
	customer_acctbal_raw.assign(capacity, 0);
	customer_nationkey.assign(capacity, 0);
	customer_address.resize(capacity);
	customer_phone.resize(capacity);
	customer_comment.resize(capacity);
}

void Q10SideStores::EnsureCustomerCapacity(int64_t custkey) {
	if (custkey <= 0) {
		return;
	}
	const auto needed = static_cast<size_t>(custkey) + 1;
	if (customer_name.size() >= needed) {
		return;
	}
	customer_active.resize(needed, 0);
	customer_name.resize(needed);
	customer_acctbal_raw.resize(needed, 0);
	customer_nationkey.resize(needed, 0);
	customer_address.resize(needed);
	customer_phone.resize(needed);
	customer_comment.resize(needed);
}

bool Q10SideStores::IsCustomerActive(int64_t custkey) const {
	if (custkey <= 0 || static_cast<size_t>(custkey) >= customer_active.size()) {
		return false;
	}
	return customer_active[static_cast<size_t>(custkey)] != 0;
}

void Q10SideStores::SetCustomer(int64_t custkey, Q10CustomerDim dim) {
	EnsureCustomerCapacity(custkey);
	const auto index = static_cast<size_t>(custkey);
	customer_active[index] = 1;
	customer_name[index] = std::move(dim.name);
	customer_acctbal_raw[index] = dim.acctbal_raw;
	customer_nationkey[index] = dim.nationkey;
	customer_address[index] = std::move(dim.address);
	customer_phone[index] = std::move(dim.phone);
	customer_comment[index] = std::move(dim.comment);
}

void Q10SideStores::DeleteCustomer(int64_t custkey) {
	if (custkey <= 0 || static_cast<size_t>(custkey) >= customer_active.size()) {
		return;
	}
	const auto index = static_cast<size_t>(custkey);
	customer_active[index] = 0;
	customer_name[index].clear();
	customer_acctbal_raw[index] = 0;
	customer_nationkey[index] = 0;
	customer_address[index].clear();
	customer_phone[index].clear();
	customer_comment[index].clear();
}

Q10AccT Q10DiscountedRevenueRaw(int64_t extendedprice_raw, int64_t discount_raw) {
	return static_cast<Q10AccT>(extendedprice_raw) * static_cast<Q10AccT>(100 - discount_raw);
}

void Q10AddRevenueRaw(Q10AccT &accumulator, int64_t extendedprice_raw, int64_t discount_raw, int8_t sign) {
#if defined(__SIZEOF_INT128__)
	accumulator += Q10DiscountedRevenueRaw(extendedprice_raw, discount_raw) * static_cast<Q10AccT>(sign);
#else
	const Q10AccT delta = Q10DiscountedRevenueRaw(extendedprice_raw, discount_raw);
	if (sign < 0) {
		if (!duckdb::Hugeint::TrySubtractInPlace(accumulator, delta)) {
			throw duckdb::OutOfRangeException("Overflow while updating Q10 revenue");
		}
	} else if (!duckdb::Hugeint::TryAddInPlace(accumulator, delta)) {
		throw duckdb::OutOfRangeException("Overflow while updating Q10 revenue");
	}
#endif
}

duckdb::hugeint_t Q10RevenueFromAccumulator(Q10AccT raw) {
	return FromAccumulator(raw);
}

void Q10AddAccumulatedRevenue(Q10AccT &target, const Q10AccT &amount, int8_t sign) {
#if defined(__SIZEOF_INT128__)
	target += amount * static_cast<Q10AccT>(sign);
#else
	if (sign < 0) {
		if (!duckdb::Hugeint::TrySubtractInPlace(target, amount)) {
			throw duckdb::OutOfRangeException("Overflow while accumulating Q10 revenue");
		}
	} else if (!duckdb::Hugeint::TryAddInPlace(target, amount)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q10 revenue");
	}
#endif
}

bool Q10IsReturnedLineitem(const duckdb::string_t &returnflag, bool is_null) {
	if (is_null || returnflag.GetSize() != 1) {
		return false;
	}
	return returnflag.GetData()[0] == 'R';
}

bool Q10IsReturnedFlag(const std::string &returnflag) {
	return returnflag.size() == 1 && returnflag[0] == 'R';
}

bool BetterQ10OutputRow(const Q10OutputRow &lhs, const Q10OutputRow &rhs) {
	if (duckdb::Hugeint::GreaterThan(lhs.revenue, rhs.revenue)) {
		return true;
	}
	if (duckdb::Hugeint::GreaterThan(rhs.revenue, lhs.revenue)) {
		return false;
	}
	return lhs.custkey < rhs.custkey;
}

duckdb::date_t Q10ComputeExclusiveEndDate(duckdb::date_t start_date) {
	duckdb::interval_t three_months;
	three_months.months = 3;
	three_months.days = 0;
	three_months.micros = 0;
	return duckdb::Interval::Add(start_date, three_months);
}

duckdb::date_t Q10MonthStartFromPartitionKey(int32_t partition_key) {
	return duckdb::Date::FromDate(ExtractYear(partition_key), ExtractMonth(partition_key), 1);
}

duckdb::date_t Q10MonthEndExclusiveFromPartitionKey(int32_t partition_key) {
	const auto year = ExtractYear(partition_key);
	const auto month = ExtractMonth(partition_key);
	if (month == 12) {
		return duckdb::Date::FromDate(year + 1, 1, 1);
	}
	return duckdb::Date::FromDate(year, month + 1, 1);
}

bool Q10MonthPartitionOverlapsWindow(int32_t partition_key, duckdb::date_t start_date, duckdb::date_t end_date) {
	const auto month_start = Q10MonthStartFromPartitionKey(partition_key);
	const auto month_end = Q10MonthEndExclusiveFromPartitionKey(partition_key);
	return month_start < end_date && month_end > start_date;
}

void Q10EnumerateWindowPartitionKeys(duckdb::date_t start_date, duckdb::date_t end_date,
                                     std::vector<int32_t> &partition_keys) {
	partition_keys.clear();
	auto cursor = start_date;
	while (cursor < end_date) {
		const auto year = duckdb::Date::ExtractYear(cursor);
		const auto month = duckdb::Date::ExtractMonth(cursor);
		const auto key = static_cast<int32_t>(year * 100 + month);
		if (partition_keys.empty() || partition_keys.back() != key) {
			partition_keys.push_back(key);
		}
		const auto month_end = Q10MonthEndExclusiveFromPartitionKey(key);
		cursor = month_end < end_date ? month_end : end_date;
	}
}

void Q10BuildMonthPartitionFromScratch(Q10MonthPartition &partition,
                                       const std::unordered_map<int64_t, Q10AccT> &revenue_by_custkey) {
	partition.entries.clear();
	partition.entries.reserve(revenue_by_custkey.size());
	for (const auto &entry : revenue_by_custkey) {
		if (entry.second == 0) {
			continue;
		}
		partition.entries.push_back(Q10MonthCustomerEntry {entry.first, entry.second});
	}
	std::sort(partition.entries.begin(), partition.entries.end(),
	          [](const Q10MonthCustomerEntry &lhs, const Q10MonthCustomerEntry &rhs) {
		          return lhs.custkey < rhs.custkey;
	          });
}

void Q10ApplyRevenueDeltaToPartition(Q10MonthPartition &partition, int64_t custkey, Q10AccT delta) {
	if (delta == 0) {
		return;
	}
	auto &entries = partition.entries;
	auto it = std::lower_bound(entries.begin(), entries.end(), custkey,
	                           [](const Q10MonthCustomerEntry &entry, int64_t key) { return entry.custkey < key; });
	if (it != entries.end() && it->custkey == custkey) {
		Q10AddAccumulatedRevenue(it->revenue_raw, delta, 1);
		if (it->revenue_raw == 0) {
			entries.erase(it);
		}
		return;
	}
	entries.insert(it, Q10MonthCustomerEntry {custkey, delta});
}

namespace {

bool BetterQ10RawSortKey(const Q10RawSortKey &lhs, const Q10RawSortKey &rhs) {
#if defined(__SIZEOF_INT128__)
	if (lhs.revenue_raw != rhs.revenue_raw) {
		return lhs.revenue_raw > rhs.revenue_raw;
	}
#else
	if (duckdb::Hugeint::GreaterThan(lhs.revenue_raw, rhs.revenue_raw)) {
		return true;
	}
	if (duckdb::Hugeint::GreaterThan(rhs.revenue_raw, lhs.revenue_raw)) {
		return false;
	}
#endif
	return lhs.custkey < rhs.custkey;
}

} // namespace

std::vector<Q10RawSortKey> ComposeQ10SortedKeys(const std::vector<int64_t> &touched_customers,
                                                const std::vector<Q10AccT> &revenue_by_customer,
                                                const Q10SideStores &side_stores) {
	std::vector<Q10RawSortKey> sort_keys;
	sort_keys.reserve(touched_customers.size());
	for (const auto custkey : touched_customers) {
		if (custkey <= 0 || static_cast<size_t>(custkey) >= revenue_by_customer.size()) {
			continue;
		}
		const auto revenue_raw = revenue_by_customer[static_cast<size_t>(custkey)];
		if (revenue_raw == 0) {
			continue;
		}
		if (!side_stores.IsCustomerActive(custkey)) {
			continue;
		}
		const auto index = static_cast<size_t>(custkey);
		const auto nationkey = side_stores.customer_nationkey[index];
		if (nationkey < 0 || nationkey >= kQ10MaxNationSlots) {
			continue;
		}
		const auto &nation_name = side_stores.nation_names[static_cast<size_t>(nationkey)];
		if (nation_name.empty()) {
			continue;
		}
		sort_keys.push_back(Q10RawSortKey {revenue_raw, custkey});
	}
	std::sort(sort_keys.begin(), sort_keys.end(), BetterQ10RawSortKey);
	return sort_keys;
}

} // namespace accelerator_codegen
