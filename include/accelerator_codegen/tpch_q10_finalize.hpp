#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"

namespace accelerator_codegen {

#if defined(__SIZEOF_INT128__)
using Q10AccT = __int128_t;
#else
using Q10AccT = duckdb::hugeint_t;
#endif

constexpr uint8_t kQ10RevenueDecimalWidth = 38;
constexpr uint8_t kQ10RevenueDecimalScale = 4;
constexpr int32_t kQ10MaxNationSlots = 25;
constexpr int64_t kQ10TpchMaxCustkey = 150000;

struct Q10CustomerDim {
	std::string name;
	int64_t acctbal_raw = 0;
	int32_t nationkey = 0;
	std::string address;
	std::string phone;
	std::string comment;
};

struct Q10SideStores {
	std::vector<uint8_t> customer_active;
	std::vector<std::string> customer_name;
	std::vector<int64_t> customer_acctbal_raw;
	std::vector<int32_t> customer_nationkey;
	std::vector<std::string> customer_address;
	std::vector<std::string> customer_phone;
	std::vector<std::string> customer_comment;
	std::array<std::string, kQ10MaxNationSlots> nation_names;

	void ClearCustomers();
	void ReserveCustomers();
	void EnsureCustomerCapacity(int64_t custkey);
	bool IsCustomerActive(int64_t custkey) const;
	void SetCustomer(int64_t custkey, Q10CustomerDim dim);
	void DeleteCustomer(int64_t custkey);
};

struct Q10MonthCustomerEntry {
	int64_t custkey = 0;
	Q10AccT revenue_raw = 0;
};

struct Q10MonthPartition {
	std::vector<Q10MonthCustomerEntry> entries;
};

struct Q10OutputRow {
	int64_t custkey = 0;
	std::string name;
	duckdb::hugeint_t revenue;
	duckdb::hugeint_t acctbal;
	std::string nation_name;
	std::string address;
	std::string phone;
	std::string comment;
};

struct Q10RawSortKey {
	Q10AccT revenue_raw = 0;
	int64_t custkey = 0;
};

Q10AccT Q10DiscountedRevenueRaw(int64_t extendedprice_raw, int64_t discount_raw);
void Q10AddRevenueRaw(Q10AccT &accumulator, int64_t extendedprice_raw, int64_t discount_raw, int8_t sign = 1);
void Q10AddAccumulatedRevenue(Q10AccT &target, const Q10AccT &amount, int8_t sign = 1);
duckdb::hugeint_t Q10RevenueFromAccumulator(Q10AccT raw);

bool Q10IsReturnedLineitem(const duckdb::string_t &returnflag, bool is_null);
bool Q10IsReturnedFlag(const std::string &returnflag);

bool BetterQ10OutputRow(const Q10OutputRow &lhs, const Q10OutputRow &rhs);

duckdb::date_t Q10ComputeExclusiveEndDate(duckdb::date_t start_date);
duckdb::date_t Q10MonthStartFromPartitionKey(int32_t partition_key);
duckdb::date_t Q10MonthEndExclusiveFromPartitionKey(int32_t partition_key);
bool Q10MonthPartitionOverlapsWindow(int32_t partition_key, duckdb::date_t start_date, duckdb::date_t end_date);
void Q10EnumerateWindowPartitionKeys(duckdb::date_t start_date, duckdb::date_t end_date,
                                     std::vector<int32_t> &partition_keys);

void Q10BuildMonthPartitionFromScratch(Q10MonthPartition &partition,
                                       const std::unordered_map<int64_t, Q10AccT> &revenue_by_custkey);
void Q10ApplyRevenueDeltaToPartition(Q10MonthPartition &partition, int64_t custkey, Q10AccT delta);

std::vector<Q10RawSortKey> ComposeQ10SortedKeys(const std::vector<int64_t> &touched_customers,
                                                const std::vector<Q10AccT> &revenue_by_customer,
                                                const Q10SideStores &side_stores);

} // namespace accelerator_codegen
