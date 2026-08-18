#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_kernel.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_runtime.hpp"
#include "tpch/q10/base/query.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <queue>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/storage_index.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;
constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr int64_t kAbsentOrderCustomer = 0;
constexpr int64_t kDefaultCustomerCapacity = 150001;
constexpr int64_t kDefaultOrderCapacity = 6000001;
constexpr int32_t kMaxNationSlots = 25;
constexpr duckdb::idx_t kCustomerCustkeyScanIndex = 0;
constexpr duckdb::idx_t kCustomerNameScanIndex = 1;
constexpr duckdb::idx_t kCustomerAcctbalScanIndex = 2;
constexpr duckdb::idx_t kCustomerNationkeyScanIndex = 3;
constexpr duckdb::idx_t kCustomerAddressScanIndex = 4;
constexpr duckdb::idx_t kCustomerPhoneScanIndex = 5;
constexpr duckdb::idx_t kCustomerCommentScanIndex = 6;
constexpr duckdb::idx_t kCustomerProjectedColumnCount = 7;

#if defined(__SIZEOF_INT128__)
using q10_acc_t = __int128_t;

duckdb::hugeint_t FromAccumulator(q10_acc_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}

void AddRevenue(q10_acc_t &accumulator, int64_t extendedprice_raw, int64_t discount_raw) {
	accumulator += static_cast<q10_acc_t>(extendedprice_raw) * static_cast<q10_acc_t>(100 - discount_raw);
}
#else
using q10_acc_t = duckdb::hugeint_t;

duckdb::hugeint_t FromAccumulator(const q10_acc_t &value) {
	return value;
}

void AddRevenue(q10_acc_t &accumulator, int64_t extendedprice_raw, int64_t discount_raw) {
	duckdb::hugeint_t price_value;
	duckdb::hugeint_t factor_value;
	duckdb::hugeint_t product_value;
	if (!duckdb::Hugeint::TryConvert(extendedprice_raw, price_value) ||
	    !duckdb::Hugeint::TryConvert(static_cast<int64_t>(100 - discount_raw), factor_value) ||
	    !duckdb::Hugeint::TryMultiply(price_value, factor_value, product_value) ||
	    !duckdb::Hugeint::TryAddInPlace(accumulator, product_value)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q10 revenue");
	}
}
#endif

std::string ReadString(const duckdb::string_t &value) {
	return std::string(value.GetData(), value.GetData() + value.GetSize());
}

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

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

//! Compact sort key: avoids moving string payloads during global sort.
struct Q10CandidateRow {
	int64_t custkey = 0;
	duckdb::hugeint_t revenue;
	int64_t acctbal_raw = 0;
	int32_t nationkey = -1;
};

struct Q10CustomerPayload {
	std::string name;
	std::string address;
	std::string phone;
	std::string comment;
};

bool BetterCandidateRow(const Q10CandidateRow &lhs, const Q10CandidateRow &rhs) {
	if (duckdb::Hugeint::GreaterThan(lhs.revenue, rhs.revenue)) {
		return true;
	}
	if (duckdb::Hugeint::GreaterThan(rhs.revenue, lhs.revenue)) {
		return false;
	}
	return lhs.custkey < rhs.custkey;
}

bool BetterOutputRow(const Q10OutputRow &lhs, const Q10OutputRow &rhs) {
	if (duckdb::Hugeint::GreaterThan(lhs.revenue, rhs.revenue)) {
		return true;
	}
	if (duckdb::Hugeint::GreaterThan(rhs.revenue, lhs.revenue)) {
		return false;
	}
	return lhs.custkey < rhs.custkey;
}

bool TryGetRowGroupNumericMinMax(const duckdb::RowGroup &row_group, const duckdb::StorageIndex &column_id,
                                 int64_t &out_min, int64_t &out_max) {
	const auto stats = row_group.GetStatistics(column_id);
	if (!stats || stats->GetStatsType() != duckdb::StatisticsType::NUMERIC_STATS) {
		return false;
	}
	out_min = duckdb::NumericStats::GetMin<int64_t>(*stats);
	out_max = duckdb::NumericStats::GetMax<int64_t>(*stats);
	return true;
}

bool SortedKeysOverlapRange(const std::vector<int64_t> &keys, int64_t range_min, int64_t range_max) {
	if (keys.empty()) {
		return false;
	}
	const auto it = std::lower_bound(keys.begin(), keys.end(), range_min);
	return it != keys.end() && *it <= range_max;
}

void AppendWorkerOrderKeys(std::vector<int64_t> &dst, std::vector<std::vector<int64_t>> &worker_keys) {
	size_t extra = 0;
	for (const auto &keys : worker_keys) {
		extra += keys.size();
	}
	dst.reserve(dst.size() + extra);
	for (auto &keys : worker_keys) {
		dst.insert(dst.end(), std::make_move_iterator(keys.begin()), std::make_move_iterator(keys.end()));
		keys.clear();
	}
}

struct Q10LineitemFusedWorkerState {
	const duckdb::RowGroup *active_row_group = nullptr;
	bool active_row_group_skipped = false;
};

class ActiveOrderStore {
public:
	void Preallocate(int64_t order_capacity) {
		const auto target = static_cast<size_t>(std::max(order_capacity, kDefaultOrderCapacity));
		if (order_active_bitmap.size() < target) {
			order_active_bitmap.resize(target, 0);
			order_custkeys.resize(target, kAbsentOrderCustomer);
		}
	}

	void StoreUnchecked(int64_t orderkey, int64_t custkey) {
		if (orderkey < 0 || static_cast<size_t>(orderkey) >= order_active_bitmap.size()) {
			return;
		}
		const auto index = static_cast<size_t>(orderkey);
		order_active_bitmap[index] = 1;
		order_custkeys[index] = custkey;
	}

	bool IsActive(int64_t orderkey) const {
		if (orderkey < 0) {
			return false;
		}
		const auto index = static_cast<size_t>(orderkey);
		return index < order_active_bitmap.size() && order_active_bitmap[index] != 0;
	}

	bool CanProbeUnchecked(int64_t orderkey) const {
		return orderkey >= 0 && static_cast<size_t>(orderkey) < order_active_bitmap.size();
	}

	bool IsActiveUnchecked(uint64_t orderkey) const {
		return order_active_bitmap[orderkey] != 0;
	}

	int64_t CustkeyUnchecked(uint64_t orderkey) const {
		return order_custkeys[orderkey];
	}

	int64_t Custkey(int64_t orderkey) const {
		if (!IsActive(orderkey)) {
			return kAbsentOrderCustomer;
		}
		return order_custkeys[static_cast<size_t>(orderkey)];
	}

	size_t LogicalBytes() const {
		return order_active_bitmap.size() + order_custkeys.size() * sizeof(int64_t);
	}

	std::vector<uint8_t> order_active_bitmap;
	std::vector<int64_t> order_custkeys;
};

class DenseRevenueStore {
public:
	bool Has(int64_t custkey) const {
		if (custkey < 0 || custkey >= capacity) {
			return false;
		}
		return has_revenue[static_cast<size_t>(custkey)] != 0;
	}

	q10_acc_t Load(int64_t custkey) const {
		if (custkey < 0 || custkey >= capacity) {
			return static_cast<q10_acc_t>(0);
		}
		return revenue[static_cast<size_t>(custkey)];
	}

	void Add(int64_t custkey, int64_t extendedprice_raw, int64_t discount_raw) {
		if (custkey < 0) {
			return;
		}
		EnsureCapacity(custkey + 1);
		const auto index = static_cast<size_t>(custkey);
		if (!has_revenue[index]) {
			has_revenue[index] = 1;
			active_keys.push_back(custkey);
		}
		AddRevenue(revenue[index], extendedprice_raw, discount_raw);
	}

	void AddAccumulator(int64_t custkey, q10_acc_t amount) {
		if (custkey < 0 || amount == 0) {
			return;
		}
		EnsureCapacity(custkey + 1);
		const auto index = static_cast<size_t>(custkey);
		if (!has_revenue[index]) {
			has_revenue[index] = 1;
			active_keys.push_back(custkey);
		}
#if defined(__SIZEOF_INT128__)
		revenue[index] += amount;
#else
		if (!duckdb::Hugeint::TryAddInPlace(revenue[index], amount)) {
			throw duckdb::OutOfRangeException("Overflow while merging Q10 revenue");
		}
#endif
	}

	const std::vector<int64_t> &ActiveKeys() const {
		return active_keys;
	}

	int64_t Capacity() const {
		return capacity;
	}

	void Initialize(int64_t target_capacity) {
		EnsureCapacity(std::max(target_capacity, kDefaultCustomerCapacity));
	}

	void MergeActiveFrom(const DenseRevenueStore &other) {
		for (const auto custkey : other.active_keys) {
			if (custkey < 0 || custkey >= other.capacity) {
				continue;
			}
			const auto index = static_cast<size_t>(custkey);
			if (!other.has_revenue[index]) {
				continue;
			}
			EnsureCapacity(custkey + 1);
			const auto merged_index = static_cast<size_t>(custkey);
			if (!has_revenue[merged_index]) {
				has_revenue[merged_index] = 1;
				active_keys.push_back(custkey);
			}
#if defined(__SIZEOF_INT128__)
			revenue[merged_index] += other.revenue[index];
#else
			if (!duckdb::Hugeint::TryAddInPlace(revenue[merged_index], other.revenue[index])) {
				throw duckdb::OutOfRangeException("Overflow while merging Q10 revenue");
			}
#endif
		}
	}

	void MergeActiveRange(const DenseRevenueStore &local, int64_t range_begin, int64_t range_end,
	                      std::vector<int64_t> &range_active) {
		for (const auto custkey : local.active_keys) {
			if (custkey < range_begin || custkey >= range_end) {
				continue;
			}
			if (custkey < 0 || custkey >= local.capacity) {
				continue;
			}
			const auto index = static_cast<size_t>(custkey);
			if (!local.has_revenue[index]) {
				continue;
			}
			const auto merged_index = static_cast<size_t>(custkey);
			if (!has_revenue[merged_index]) {
				has_revenue[merged_index] = 1;
				range_active.push_back(custkey);
			}
#if defined(__SIZEOF_INT128__)
			revenue[merged_index] += local.revenue[index];
#else
			if (!duckdb::Hugeint::TryAddInPlace(revenue[merged_index], local.revenue[index])) {
				throw duckdb::OutOfRangeException("Overflow while merging Q10 revenue");
			}
#endif
		}
	}

	void AppendActiveKeys(const std::vector<int64_t> &keys) {
		active_keys.insert(active_keys.end(), keys.begin(), keys.end());
	}

	size_t LogicalBytes() const {
		return revenue.size() * sizeof(q10_acc_t) + has_revenue.size() + active_keys.size() * sizeof(int64_t);
	}

private:
	void EnsureCapacity(int64_t new_capacity) {
		if (new_capacity <= capacity) {
			return;
		}
		int64_t target = new_capacity;
		if (capacity > 0) {
			const int64_t doubled = capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		} else {
			target = std::max(target, kDefaultCustomerCapacity);
		}
		revenue.resize(static_cast<size_t>(target), 0);
		has_revenue.resize(static_cast<size_t>(target), 0);
		capacity = target;
	}

	int64_t capacity = 0;
	std::vector<q10_acc_t> revenue;
	std::vector<uint8_t> has_revenue;
	std::vector<int64_t> active_keys;
};

void MergeRevenueStores(DenseRevenueStore &global, const DenseRevenueStore &local) {
	global.MergeActiveFrom(local);
}

void MergeWorkerRevenueStores(const std::vector<DenseRevenueStore *> &workers, DenseRevenueStore &merged,
                              int64_t merged_capacity, duckdb::idx_t merge_workers) {
	if (workers.empty()) {
		return;
	}
	merged.Initialize(merged_capacity);
	if (workers.size() == 1) {
		merged.MergeActiveFrom(*workers[0]);
		return;
	}
	if (merge_workers <= 1) {
		for (const auto *worker : workers) {
			merged.MergeActiveFrom(*worker);
		}
		return;
	}

	const auto workers_for_merge = std::min<duckdb::idx_t>(merge_workers, static_cast<duckdb::idx_t>(merged.Capacity()));
	const auto range_size = (static_cast<size_t>(merged.Capacity()) + workers_for_merge - 1) / workers_for_merge;
	std::vector<std::thread> threads;
	threads.reserve(workers_for_merge > 0 ? workers_for_merge - 1 : 0);
	std::vector<std::vector<int64_t>> range_actives(workers_for_merge);
	std::mutex error_mutex;
	std::exception_ptr error;

	auto run_range = [&](duckdb::idx_t range_id) {
		try {
			const auto range_begin = static_cast<int64_t>(range_id * range_size);
			const auto range_end =
			    std::min<int64_t>(merged.Capacity(), static_cast<int64_t>((range_id + 1) * range_size));
			if (range_begin >= range_end) {
				return;
			}
			for (const auto *worker : workers) {
				merged.MergeActiveRange(*worker, range_begin, range_end, range_actives[range_id]);
			}
		} catch (...) {
			std::lock_guard<std::mutex> lock(error_mutex);
			if (!error) {
				error = std::current_exception();
			}
		}
	};

	for (duckdb::idx_t range_id = 1; range_id < workers_for_merge; range_id++) {
		threads.emplace_back([&, range_id]() { run_range(range_id); });
	}
	run_range(0);
	for (auto &thread : threads) {
		thread.join();
	}
	if (error) {
		std::rethrow_exception(error);
	}
	for (auto &active : range_actives) {
		merged.AppendActiveKeys(active);
	}
}

struct Q10FrozenState {
	std::array<std::string, kMaxNationSlots> nation_names;
	ActiveOrderStore order_customers;
};

struct ActiveCustomerIndex {
	std::vector<uint8_t> bitmap;
	int64_t capacity = 0;

	void Build(const DenseRevenueStore &revenue) {
		bitmap.clear();
		capacity = 0;
		for (const auto custkey : revenue.ActiveKeys()) {
			if (custkey < 0) {
				continue;
			}
			const auto required = custkey + 1;
			if (required > capacity) {
				int64_t target = required;
				if (capacity > 0) {
					const int64_t doubled = capacity * 2;
					if (doubled > target) {
						target = doubled;
					}
				} else {
					target = std::max(target, kDefaultCustomerCapacity);
				}
				bitmap.resize(static_cast<size_t>(target), 0);
				capacity = target;
			}
			bitmap[static_cast<size_t>(custkey)] = 1;
		}
	}

	bool IsActive(int64_t custkey) const {
		return custkey >= 0 && custkey < capacity && bitmap[static_cast<size_t>(custkey)] != 0;
	}

	size_t LogicalBytes() const {
		return bitmap.size();
	}
};

struct Q10ScanIndices {
	duckdb::idx_t orders_orderkey = 0;
	duckdb::idx_t orders_custkey = 1;
	duckdb::idx_t orders_orderdate = 2;
	duckdb::idx_t lineitem_orderkey = 0;
	duckdb::idx_t lineitem_extendedprice = 1;
	duckdb::idx_t lineitem_discount = 2;
	duckdb::idx_t lineitem_returnflag = 3;
	duckdb::idx_t customer_custkey = 0;
	duckdb::idx_t customer_name = 1;
	duckdb::idx_t customer_acctbal = 2;
	duckdb::idx_t customer_nationkey = 3;
	duckdb::idx_t customer_address = 4;
	duckdb::idx_t customer_phone = 5;
	duckdb::idx_t customer_comment = 6;
};

struct PassProfile {
	size_t rows_in = 0;
	size_t rows_out = 0;
	size_t flat_rows = 0;
	size_t fallback_rows = 0;
	int64_t process_ns = 0;
};

struct Q10ParallelProfile {
	int64_t nation_ns = 0;
	int64_t orders_ns = 0;
	int64_t lineitem_ns = 0;
	int64_t revenue_merge_ns = 0;
	int64_t active_customer_ns = 0;
	int64_t customer_ns = 0;
	int64_t output_merge_ns = 0;
	int64_t output_sort_ns = 0;
	int64_t output_materialize_ns = 0;
	size_t worker_count = 0;
	size_t orders_date_filtered_rows = 0;
	size_t lineitem_return_filtered_rows = 0;
	size_t lineitem_order_matched_rows = 0;
	size_t lineitem_payload_rows = 0;
	size_t customer_rows_scanned = 0;
	size_t customer_active_survivors = 0;
	size_t customer_strings_materialized = 0;
	size_t active_customers = 0;
	size_t output_rows_before_sort = 0;
	size_t orders_bitmap_bytes = 0;
	size_t revenue_bytes = 0;
	size_t revenue_worker_bytes = 0;
	size_t active_customer_bitmap_bytes = 0;
	size_t orders_flat_rows = 0;
	size_t lineitem_flat_rows = 0;
	size_t customer_flat_rows = 0;
	size_t orders_fallback_rows = 0;
	size_t lineitem_fallback_rows = 0;
	size_t customer_fallback_rows = 0;
	int64_t min_worker_lineitem_ns = 0;
	int64_t max_worker_lineitem_ns = 0;
	int64_t min_worker_customer_ns = 0;
	int64_t max_worker_customer_ns = 0;
};

void BuildNationState(duckdb::ClientContext &context, const duckdb::FunctionData &nation_bind, Q10FrozenState &state) {
	auto source = CreateNativeChunkSource(context, nation_bind);
	while (source->Next()) {
		auto &chunk = source->Chunk();
		duckdb::UnifiedVectorFormat nationkey_format;
		duckdb::UnifiedVectorFormat name_format;
		chunk.data[Q10NationChunkLayout::NATIONKEY_INDEX].ToUnifiedFormat(chunk.size(), nationkey_format);
		chunk.data[Q10NationChunkLayout::NAME_INDEX].ToUnifiedFormat(chunk.size(), name_format);
		auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto nationkey_index = nationkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			if ((!nationkey_format.validity.AllValid() && !nationkey_format.validity.RowIsValidUnsafe(nationkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index))) {
				continue;
			}
			const auto nationkey = nationkeys[nationkey_index];
			if (nationkey < 0 || nationkey >= kMaxNationSlots) {
				continue;
			}
			const auto name_value = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index];
			state.nation_names[static_cast<size_t>(nationkey)] = ReadString(name_value);
		}
	}
}

void SkipOrdersPayload(duckdb::RowGroup &row_group, duckdb::CollectionScanState &scan_state,
                       const Q10ScanIndices &indices) {
	row_group.SkipProjectedColumn(scan_state, indices.orders_orderkey);
	row_group.SkipProjectedColumn(scan_state, indices.orders_custkey);
	row_group.SkipProjectedColumn(scan_state, indices.orders_orderdate);
}

void AccumulateFusedOrders(duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
                           duckdb::RowGroup &row_group, duckdb::SelectionVector &sel, duckdb::idx_t count,
                           const Q10ScanIndices &indices, ActiveOrderStore &orders, PassProfile &profile) {
	profile.rows_in += count;
	if (count == 0) {
		SkipOrdersPayload(row_group, scan_state, indices);
		return;
	}

	duckdb::Vector orderkey_vec(duckdb::LogicalType::BIGINT);
	duckdb::Vector custkey_vec(duckdb::LogicalType::BIGINT);
	row_group.SelectProjectedColumn(transaction, scan_state, indices.orders_orderkey, sel, count, orderkey_vec,
	                              duckdb::UpdateScanType::STANDARD);
	row_group.SelectProjectedColumn(transaction, scan_state, indices.orders_custkey, sel, count, custkey_vec,
	                              duckdb::UpdateScanType::STANDARD);
	row_group.SkipProjectedColumn(scan_state, indices.orders_orderdate);

	if (IsFlatAllValid(orderkey_vec) && IsFlatAllValid(custkey_vec)) {
		profile.flat_rows += count;
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vec);
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vec);
		for (duckdb::idx_t row = 0; row < count; row++) {
			orders.StoreUnchecked(orderkeys[row], custkeys[row]);
			profile.rows_out++;
		}
		return;
	}

	profile.fallback_rows += count;
	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat custkey_format;
	orderkey_vec.ToUnifiedFormat(count, orderkey_format);
	custkey_vec.ToUnifiedFormat(count, custkey_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto custkey_index = custkey_format.sel->get_index(row);
		if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
		    (!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index))) {
			continue;
		}
		orders.StoreUnchecked(orderkeys[orderkey_index], custkeys[custkey_index]);
		profile.rows_out++;
	}
}

void SkipLineitemReturnflagIfPresent(duckdb::RowGroup &row_group, duckdb::CollectionScanState &scan_state,
                                     const Q10ScanIndices &indices) {
	const auto &column_ids = scan_state.GetColumnIds();
	if (column_ids.size() > indices.lineitem_returnflag) {
		row_group.SkipProjectedColumn(scan_state, indices.lineitem_returnflag);
	}
}

void SkipLineitemPayload(duckdb::RowGroup &row_group, duckdb::CollectionScanState &scan_state,
                         const Q10ScanIndices &indices) {
	row_group.SkipProjectedColumn(scan_state, indices.lineitem_orderkey);
	row_group.SkipProjectedColumn(scan_state, indices.lineitem_extendedprice);
	row_group.SkipProjectedColumn(scan_state, indices.lineitem_discount);
	SkipLineitemReturnflagIfPresent(row_group, scan_state, indices);
}

void AccumulateFusedLineitem(duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
                             duckdb::RowGroup &row_group, duckdb::SelectionVector &sel, duckdb::idx_t count,
                             const Q10ScanIndices &indices, const ActiveOrderStore &orders,
                             const std::vector<int64_t> &qualifying_order_keys, Q10LineitemFusedWorkerState &fused_state,
                             DenseRevenueStore &agg, PassProfile &profile) {
	if (qualifying_order_keys.empty()) {
		return;
	}

	const auto &column_ids = scan_state.GetColumnIds();
	if (&row_group != fused_state.active_row_group) {
		fused_state.active_row_group = &row_group;
		fused_state.active_row_group_skipped = false;
		if (column_ids.size() > indices.lineitem_orderkey) {
			int64_t order_min = 0;
			int64_t order_max = 0;
			if (TryGetRowGroupNumericMinMax(row_group, column_ids[indices.lineitem_orderkey], order_min, order_max) &&
			    !SortedKeysOverlapRange(qualifying_order_keys, order_min, order_max)) {
				fused_state.active_row_group_skipped = true;
			}
		}
	}

	if (fused_state.active_row_group_skipped) {
		SkipLineitemPayload(row_group, scan_state, indices);
		return;
	}

	profile.rows_in += count;
	if (count == 0) {
		SkipLineitemPayload(row_group, scan_state, indices);
		return;
	}

	duckdb::Vector orderkey_vec(duckdb::LogicalType::BIGINT);
	row_group.SelectProjectedColumn(transaction, scan_state, indices.lineitem_orderkey, sel, count, orderkey_vec,
	                                duckdb::UpdateScanType::STANDARD);

	struct OrderSurvivor {
		duckdb::idx_t batch_row = 0;
		int64_t custkey = 0;
	};
	OrderSurvivor survivors[STANDARD_VECTOR_SIZE];
	duckdb::idx_t survivor_count = 0;

	const auto collect_survivor = [&](duckdb::idx_t batch_row, int64_t orderkey) {
		int64_t custkey = kAbsentOrderCustomer;
		if (orders.CanProbeUnchecked(orderkey)) {
			if (!orders.IsActiveUnchecked(static_cast<uint64_t>(orderkey))) {
				return;
			}
			custkey = orders.CustkeyUnchecked(static_cast<uint64_t>(orderkey));
		} else if (!orders.IsActive(orderkey)) {
			return;
		} else {
			custkey = orders.Custkey(orderkey);
		}
		if (custkey == kAbsentOrderCustomer) {
			return;
		}
		survivors[survivor_count++] = OrderSurvivor {batch_row, custkey};
	};

	if (IsFlatAllValid(orderkey_vec)) {
		profile.flat_rows += count;
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vec);
		for (duckdb::idx_t row = 0; row < count; row++) {
			collect_survivor(row, orderkeys[row]);
		}
	} else {
		profile.fallback_rows += count;
		duckdb::UnifiedVectorFormat orderkey_format;
		orderkey_vec.ToUnifiedFormat(count, orderkey_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			if (!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) {
				continue;
			}
			collect_survivor(row, orderkeys[orderkey_index]);
		}
	}

	if (survivor_count == 0) {
		row_group.SkipProjectedColumn(scan_state, indices.lineitem_extendedprice);
		row_group.SkipProjectedColumn(scan_state, indices.lineitem_discount);
		SkipLineitemReturnflagIfPresent(row_group, scan_state, indices);
		return;
	}

	profile.rows_out += survivor_count;

	duckdb::SelectionVector payload_sel(survivor_count);
	for (duckdb::idx_t row = 0; row < survivor_count; row++) {
		payload_sel.set_index(row, survivors[row].batch_row);
	}

	duckdb::Vector extendedprice_vec(duckdb::LogicalType::DECIMAL(15, 2));
	duckdb::Vector discount_vec(duckdb::LogicalType::DECIMAL(15, 2));
	row_group.SelectProjectedColumn(transaction, scan_state, indices.lineitem_extendedprice, payload_sel, survivor_count,
	                                extendedprice_vec, duckdb::UpdateScanType::STANDARD);
	row_group.SelectProjectedColumn(transaction, scan_state, indices.lineitem_discount, payload_sel, survivor_count,
	                                discount_vec, duckdb::UpdateScanType::STANDARD);
	SkipLineitemReturnflagIfPresent(row_group, scan_state, indices);

	if (IsFlatAllValid(extendedprice_vec) && IsFlatAllValid(discount_vec)) {
		auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vec);
		auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vec);
		for (duckdb::idx_t row = 0; row < survivor_count; row++) {
			agg.Add(survivors[row].custkey, extendedprices[row], discounts[row]);
		}
		return;
	}

	duckdb::UnifiedVectorFormat extendedprice_format;
	duckdb::UnifiedVectorFormat discount_format;
	extendedprice_vec.ToUnifiedFormat(survivor_count, extendedprice_format);
	discount_vec.ToUnifiedFormat(survivor_count, discount_format);
	auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
	auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
	for (duckdb::idx_t row = 0; row < survivor_count; row++) {
		const auto extendedprice_index = extendedprice_format.sel->get_index(row);
		const auto discount_index = discount_format.sel->get_index(row);
		if ((!extendedprice_format.validity.AllValid() &&
		     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
		    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index))) {
			continue;
		}
		agg.Add(survivors[row].custkey, extendedprices[extendedprice_index], discounts[discount_index]);
	}
}

bool IsDictionaryOverFlat(duckdb::Vector &vector) {
	if (vector.GetVectorType() != duckdb::VectorType::DICTIONARY_VECTOR) {
		return false;
	}
	return duckdb::DictionaryVector::Child(vector).GetVectorType() == duckdb::VectorType::FLAT_VECTOR;
}

void AccumulateCustomerChunk(duckdb::DataChunk &chunk, const ActiveCustomerIndex &active_customers,
                             const DenseRevenueStore &revenue, const Q10FrozenState &frozen,
                             std::vector<Q10CandidateRow> &local_candidates,
                             std::vector<Q10CustomerPayload> &local_payloads, PassProfile &profile) {
	profile.rows_in += chunk.size();
	auto &custkey_vector = chunk.data[Q10CustomerChunkLayout::CUSTKEY_INDEX];
	auto &name_vector = chunk.data[Q10CustomerChunkLayout::NAME_INDEX];
	auto &acctbal_vector = chunk.data[Q10CustomerChunkLayout::ACCTBAL_INDEX];
	auto &nationkey_vector = chunk.data[Q10CustomerChunkLayout::NATIONKEY_INDEX];
	auto &address_vector = chunk.data[Q10CustomerChunkLayout::ADDRESS_INDEX];
	auto &phone_vector = chunk.data[Q10CustomerChunkLayout::PHONE_INDEX];
	auto &comment_vector = chunk.data[Q10CustomerChunkLayout::COMMENT_INDEX];

	const auto emit_row = [&](int64_t custkey, duckdb::string_t name, int64_t acctbal_raw, int32_t nationkey,
	                          duckdb::string_t address, duckdb::string_t phone, duckdb::string_t comment) {
		if (nationkey < 0 || nationkey >= kMaxNationSlots) {
			return;
		}
		if (frozen.nation_names[static_cast<size_t>(nationkey)].empty()) {
			return;
		}
		Q10CandidateRow candidate;
		candidate.custkey = custkey;
		candidate.revenue = FromAccumulator(revenue.Load(custkey));
		candidate.acctbal_raw = acctbal_raw;
		candidate.nationkey = nationkey;
		local_candidates.push_back(candidate);
		local_payloads.push_back(
		    Q10CustomerPayload {ReadString(name), ReadString(address), ReadString(phone), ReadString(comment)});
		profile.rows_out++;
	};

	if (IsFlatAllValid(custkey_vector) && IsFlatAllValid(name_vector) && IsFlatAllValid(acctbal_vector) &&
	    IsFlatAllValid(nationkey_vector) && IsFlatAllValid(address_vector) && IsFlatAllValid(phone_vector) &&
	    IsFlatAllValid(comment_vector)) {
		profile.flat_rows += chunk.size();
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
		auto names = duckdb::FlatVector::GetData<duckdb::string_t>(name_vector);
		auto acctbals = duckdb::FlatVector::GetData<int64_t>(acctbal_vector);
		auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
		auto addresses = duckdb::FlatVector::GetData<duckdb::string_t>(address_vector);
		auto phones = duckdb::FlatVector::GetData<duckdb::string_t>(phone_vector);
		auto comments = duckdb::FlatVector::GetData<duckdb::string_t>(comment_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			if (!active_customers.IsActive(custkeys[row])) {
				continue;
			}
			emit_row(custkeys[row], names[row], acctbals[row], nationkeys[row], addresses[row], phones[row],
			         comments[row]);
		}
		return;
	}

	profile.fallback_rows += chunk.size();
	duckdb::UnifiedVectorFormat formats[7];
	for (duckdb::idx_t col = 0; col < 7; col++) {
		chunk.data[col].ToUnifiedFormat(chunk.size(), formats[col]);
	}
	auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(formats[Q10CustomerChunkLayout::CUSTKEY_INDEX]);
	auto acctbals = duckdb::UnifiedVectorFormat::GetData<int64_t>(formats[Q10CustomerChunkLayout::ACCTBAL_INDEX]);
	auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(formats[Q10CustomerChunkLayout::NATIONKEY_INDEX]);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto indices = std::array<duckdb::idx_t, 7> {
		    formats[0].sel->get_index(row), formats[1].sel->get_index(row), formats[2].sel->get_index(row),
		    formats[3].sel->get_index(row), formats[4].sel->get_index(row), formats[5].sel->get_index(row),
		    formats[6].sel->get_index(row)};
		for (duckdb::idx_t col = 0; col < 7; col++) {
			if (!formats[col].validity.AllValid() && !formats[col].validity.RowIsValidUnsafe(indices[col])) {
				goto skip_row;
			}
		}
		if (!active_customers.IsActive(custkeys[indices[0]])) {
			goto skip_row;
		}
		emit_row(custkeys[indices[0]],
		         duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[1])[indices[1]], acctbals[indices[2]],
		         nationkeys[indices[3]],
		         duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[4])[indices[4]],
		         duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[5])[indices[5]],
		         duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[6])[indices[6]]);
	skip_row:;
	}
}

void AccumulateFusedCustomer(duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
                             duckdb::RowGroup &row_group, duckdb::SelectionVector &sel, duckdb::idx_t count,
                             const ActiveCustomerIndex &active_customers, const DenseRevenueStore &revenue,
                             const Q10FrozenState &frozen, std::vector<Q10CandidateRow> &local_candidates,
                             std::vector<Q10CustomerPayload> &local_payloads, PassProfile &profile) {
	if (count == 0) {
		return;
	}
	const auto &column_ids = scan_state.GetColumnIds();
	if (column_ids.size() != kCustomerProjectedColumnCount) {
		throw duckdb::InternalException("Q10 parallel fused customer expected %llu projected columns, got %llu",
		                                static_cast<unsigned long long>(kCustomerProjectedColumnCount),
		                                static_cast<unsigned long long>(column_ids.size()));
	}
	profile.rows_in += count;

	struct CustomerMatch {
		duckdb::idx_t batch_row = 0;
		int64_t custkey = 0;
	};

	const auto emit_match = [&](int64_t custkey, duckdb::string_t name, int64_t acctbal_raw, int32_t nationkey,
	                            duckdb::string_t address, duckdb::string_t phone, duckdb::string_t comment) {
		if (nationkey < 0 || nationkey >= kMaxNationSlots) {
			return;
		}
		if (frozen.nation_names[static_cast<size_t>(nationkey)].empty()) {
			return;
		}
		Q10CandidateRow candidate;
		candidate.custkey = custkey;
		candidate.revenue = FromAccumulator(revenue.Load(custkey));
		candidate.acctbal_raw = acctbal_raw;
		candidate.nationkey = nationkey;
		local_candidates.push_back(candidate);
		local_payloads.push_back(
		    Q10CustomerPayload {ReadString(name), ReadString(address), ReadString(phone), ReadString(comment)});
		profile.rows_out++;
	};

	duckdb::Vector custkey_vec(duckdb::LogicalType::BIGINT);
	row_group.SelectProjectedColumn(transaction, scan_state, kCustomerCustkeyScanIndex, sel, count, custkey_vec,
	                                duckdb::UpdateScanType::STANDARD);

	CustomerMatch matches[STANDARD_VECTOR_SIZE];
	duckdb::idx_t match_count = 0;

	if (IsFlatAllValid(custkey_vec)) {
		profile.flat_rows += count;
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vec);
		const bool identity = sel.data() == nullptr;
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto vector_index = identity ? row : sel.get_index(row);
			const auto custkey = custkeys[vector_index];
			if (!active_customers.IsActive(custkey)) {
				continue;
			}
			matches[match_count++] = CustomerMatch {row, custkey};
		}
	} else {
		profile.fallback_rows += count;
		duckdb::UnifiedVectorFormat custkey_format;
		custkey_vec.ToUnifiedFormat(count, custkey_format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto vector_index = custkey_format.sel->get_index(row);
			if (!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(vector_index)) {
				continue;
			}
			const auto custkey = custkeys[vector_index];
			if (!active_customers.IsActive(custkey)) {
				continue;
			}
			matches[match_count++] = CustomerMatch {row, custkey};
		}
	}

	if (match_count == 0) {
		for (duckdb::idx_t col = 1; col < kCustomerProjectedColumnCount; col++) {
			row_group.SkipProjectedColumn(scan_state, col);
		}
		return;
	}

	duckdb::SelectionVector match_sel(match_count);
	for (duckdb::idx_t match_index = 0; match_index < match_count; match_index++) {
		match_sel.set_index(match_index, matches[match_index].batch_row);
	}

	duckdb::Vector name_vec(duckdb::LogicalType::VARCHAR);
	duckdb::Vector acctbal_vec(duckdb::LogicalType::DECIMAL(15, 2));
	duckdb::Vector nationkey_vec(duckdb::LogicalType::INTEGER);
	duckdb::Vector address_vec(duckdb::LogicalType::VARCHAR);
	duckdb::Vector phone_vec(duckdb::LogicalType::VARCHAR);
	duckdb::Vector comment_vec(duckdb::LogicalType::VARCHAR);
	row_group.SelectProjectedColumn(transaction, scan_state, kCustomerNameScanIndex, match_sel, match_count, name_vec,
	                                duckdb::UpdateScanType::STANDARD);
	row_group.SelectProjectedColumn(transaction, scan_state, kCustomerAcctbalScanIndex, match_sel, match_count,
	                                acctbal_vec, duckdb::UpdateScanType::STANDARD);
	row_group.SelectProjectedColumn(transaction, scan_state, kCustomerNationkeyScanIndex, match_sel, match_count,
	                                nationkey_vec, duckdb::UpdateScanType::STANDARD);
	row_group.SelectProjectedColumn(transaction, scan_state, kCustomerAddressScanIndex, match_sel, match_count,
	                                address_vec, duckdb::UpdateScanType::STANDARD);
	row_group.SelectProjectedColumn(transaction, scan_state, kCustomerPhoneScanIndex, match_sel, match_count, phone_vec,
	                                duckdb::UpdateScanType::STANDARD);
	row_group.SelectProjectedColumn(transaction, scan_state, kCustomerCommentScanIndex, match_sel, match_count,
	                                comment_vec, duckdb::UpdateScanType::STANDARD);

	if (IsFlatAllValid(name_vec) && IsFlatAllValid(acctbal_vec) && IsFlatAllValid(nationkey_vec) &&
	    IsFlatAllValid(address_vec) && IsFlatAllValid(phone_vec) && IsFlatAllValid(comment_vec)) {
		auto names = duckdb::FlatVector::GetData<duckdb::string_t>(name_vec);
		auto acctbals = duckdb::FlatVector::GetData<int64_t>(acctbal_vec);
		auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vec);
		auto addresses = duckdb::FlatVector::GetData<duckdb::string_t>(address_vec);
		auto phones = duckdb::FlatVector::GetData<duckdb::string_t>(phone_vec);
		auto comments = duckdb::FlatVector::GetData<duckdb::string_t>(comment_vec);
		for (duckdb::idx_t match_index = 0; match_index < match_count; match_index++) {
			const auto &match = matches[match_index];
			emit_match(match.custkey, names[match_index], acctbals[match_index], nationkeys[match_index],
			           addresses[match_index], phones[match_index], comments[match_index]);
		}
		return;
	}

	duckdb::UnifiedVectorFormat formats[6];
	name_vec.ToUnifiedFormat(match_count, formats[0]);
	acctbal_vec.ToUnifiedFormat(match_count, formats[1]);
	nationkey_vec.ToUnifiedFormat(match_count, formats[2]);
	address_vec.ToUnifiedFormat(match_count, formats[3]);
	phone_vec.ToUnifiedFormat(match_count, formats[4]);
	comment_vec.ToUnifiedFormat(match_count, formats[5]);
	auto acctbals = duckdb::UnifiedVectorFormat::GetData<int64_t>(formats[1]);
	auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(formats[2]);
	for (duckdb::idx_t match_index = 0; match_index < match_count; match_index++) {
		const auto indices = std::array<duckdb::idx_t, 6> {
		    formats[0].sel->get_index(match_index), formats[1].sel->get_index(match_index),
		    formats[2].sel->get_index(match_index), formats[3].sel->get_index(match_index),
		    formats[4].sel->get_index(match_index), formats[5].sel->get_index(match_index)};
		for (duckdb::idx_t col = 0; col < 6; col++) {
			if (!formats[col].validity.AllValid() && !formats[col].validity.RowIsValidUnsafe(indices[col])) {
				goto skip_match;
			}
		}
		emit_match(matches[match_index].custkey,
		           duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[0])[indices[0]], acctbals[indices[1]],
		           nationkeys[indices[2]],
		           duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[3])[indices[3]],
		           duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[4])[indices[4]],
		           duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[5])[indices[5]]);
	skip_match:;
	}
}

void AccumulateOrdersChunk(duckdb::DataChunk &chunk, ActiveOrderStore &orders, PassProfile &profile,
                           std::vector<int64_t> *out_order_keys = nullptr) {
	profile.rows_in += chunk.size();
	auto &orderkey_vector = chunk.data[Q10OrdersChunkLayout::ORDERKEY_INDEX];
	auto &custkey_vector = chunk.data[Q10OrdersChunkLayout::CUSTKEY_INDEX];
	if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(custkey_vector)) {
		profile.flat_rows += chunk.size();
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			orders.StoreUnchecked(orderkeys[row], custkeys[row]);
			if (out_order_keys != nullptr) {
				out_order_keys->push_back(orderkeys[row]);
			}
			profile.rows_out++;
		}
		return;
	}
	if (IsDictionaryOverFlat(orderkey_vector) && IsDictionaryOverFlat(custkey_vector)) {
		profile.flat_rows += chunk.size();
		auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
		auto &custkey_sel = duckdb::DictionaryVector::SelVector(custkey_vector);
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(custkey_vector));
		auto &orderkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector));
		auto &custkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(custkey_vector));
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto orderkey_index = orderkey_sel.get_index(row);
			const auto custkey_index = custkey_sel.get_index(row);
			if (!orderkey_validity.RowIsValid(orderkey_index) || !custkey_validity.RowIsValid(custkey_index)) {
				continue;
			}
			orders.StoreUnchecked(orderkeys[orderkey_index], custkeys[custkey_index]);
			if (out_order_keys != nullptr) {
				out_order_keys->push_back(orderkeys[orderkey_index]);
			}
			profile.rows_out++;
		}
		return;
	}
	profile.fallback_rows += chunk.size();
	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat custkey_format;
	orderkey_vector.ToUnifiedFormat(chunk.size(), orderkey_format);
	custkey_vector.ToUnifiedFormat(chunk.size(), custkey_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto custkey_index = custkey_format.sel->get_index(row);
		if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
		    (!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index))) {
			continue;
		}
		orders.StoreUnchecked(orderkeys[orderkey_index], custkeys[custkey_index]);
		if (out_order_keys != nullptr) {
			out_order_keys->push_back(orderkeys[orderkey_index]);
		}
		profile.rows_out++;
	}
}

void AccumulateLineitemChunk(duckdb::DataChunk &chunk, const ActiveOrderStore &orders, DenseRevenueStore &agg,
                             PassProfile &profile) {
	profile.rows_in += chunk.size();
	auto &orderkey_vector = chunk.data[Q10LineitemChunkLayout::ORDERKEY_INDEX];
	auto &extendedprice_vector = chunk.data[Q10LineitemChunkLayout::EXTENDEDPRICE_INDEX];
	auto &discount_vector = chunk.data[Q10LineitemChunkLayout::DISCOUNT_INDEX];
	if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(extendedprice_vector) && IsFlatAllValid(discount_vector)) {
		profile.flat_rows += chunk.size();
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vector);
		auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto orderkey = orderkeys[row];
			int64_t custkey = kAbsentOrderCustomer;
			if (orders.CanProbeUnchecked(orderkey)) {
				if (!orders.IsActiveUnchecked(static_cast<uint64_t>(orderkey))) {
					continue;
				}
				custkey = orders.CustkeyUnchecked(static_cast<uint64_t>(orderkey));
			} else if (!orders.IsActive(orderkey)) {
				continue;
			} else {
				custkey = orders.Custkey(orderkey);
			}
			if (custkey == kAbsentOrderCustomer) {
				continue;
			}
			agg.Add(custkey, extendedprices[row], discounts[row]);
			profile.rows_out++;
		}
		return;
	}
	if (IsDictionaryOverFlat(orderkey_vector) && IsDictionaryOverFlat(extendedprice_vector) &&
	    IsDictionaryOverFlat(discount_vector)) {
		profile.flat_rows += chunk.size();
		auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
		auto &extendedprice_sel = duckdb::DictionaryVector::SelVector(extendedprice_vector);
		auto &discount_sel = duckdb::DictionaryVector::SelVector(discount_vector);
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
		auto extendedprices =
		    duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(extendedprice_vector));
		auto discounts = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(discount_vector));
		auto &orderkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector));
		auto &extendedprice_validity =
		    duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(extendedprice_vector));
		auto &discount_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(discount_vector));
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto orderkey_index = orderkey_sel.get_index(row);
			const auto extendedprice_index = extendedprice_sel.get_index(row);
			const auto discount_index = discount_sel.get_index(row);
			if (!orderkey_validity.RowIsValid(orderkey_index) || !extendedprice_validity.RowIsValid(extendedprice_index) ||
			    !discount_validity.RowIsValid(discount_index)) {
				continue;
			}
			const auto orderkey = orderkeys[orderkey_index];
			if (!orders.IsActive(orderkey)) {
				continue;
			}
			const auto custkey = orders.Custkey(orderkey);
			if (custkey == kAbsentOrderCustomer) {
				continue;
			}
			agg.Add(custkey, extendedprices[extendedprice_index], discounts[discount_index]);
			profile.rows_out++;
		}
		return;
	}
	profile.fallback_rows += chunk.size();
	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat extendedprice_format;
	duckdb::UnifiedVectorFormat discount_format;
	orderkey_vector.ToUnifiedFormat(chunk.size(), orderkey_format);
	extendedprice_vector.ToUnifiedFormat(chunk.size(), extendedprice_format);
	discount_vector.ToUnifiedFormat(chunk.size(), discount_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
	auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto extendedprice_index = extendedprice_format.sel->get_index(row);
		const auto discount_index = discount_format.sel->get_index(row);
		if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
		    (!extendedprice_format.validity.AllValid() &&
		     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
		    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index))) {
			continue;
		}
		const auto orderkey = orderkeys[orderkey_index];
		if (!orders.IsActive(orderkey)) {
			continue;
		}
		const auto custkey = orders.Custkey(orderkey);
		if (custkey == kAbsentOrderCustomer) {
			continue;
		}
		agg.Add(custkey, extendedprices[extendedprice_index], discounts[discount_index]);
		profile.rows_out++;
	}
}

void ParallelSortCandidateIndices(std::vector<size_t> &sort_order, const std::vector<Q10CandidateRow> &candidates,
                                  duckdb::idx_t sort_workers) {
	const auto comparator = [&](size_t lhs, size_t rhs) { return BetterCandidateRow(candidates[lhs], candidates[rhs]); };
	if (sort_order.size() <= 1 || sort_workers <= 1) {
		std::sort(sort_order.begin(), sort_order.end(), comparator);
		return;
	}

	const auto workers = std::min<duckdb::idx_t>(sort_workers, static_cast<duckdb::idx_t>(sort_order.size()));
	const auto chunk_size = (sort_order.size() + workers - 1) / workers;
	std::vector<std::vector<size_t>> sorted_chunks(workers);
	std::vector<std::thread> threads;
	threads.reserve(workers > 0 ? workers - 1 : 0);

	auto sort_chunk = [&](duckdb::idx_t worker_id) {
		std::sort(sorted_chunks[worker_id].begin(), sorted_chunks[worker_id].end(), comparator);
	};

	for (duckdb::idx_t worker_id = 0; worker_id < workers; worker_id++) {
		const auto begin = static_cast<size_t>(worker_id * chunk_size);
		const auto end = std::min(sort_order.size(), static_cast<size_t>((worker_id + 1) * chunk_size));
		if (begin >= end) {
			continue;
		}
		sorted_chunks[worker_id].assign(sort_order.begin() + static_cast<std::ptrdiff_t>(begin),
		                                sort_order.begin() + static_cast<std::ptrdiff_t>(end));
	}

	for (duckdb::idx_t worker_id = 1; worker_id < workers; worker_id++) {
		if (!sorted_chunks[worker_id].empty()) {
			threads.emplace_back([&, worker_id]() { sort_chunk(worker_id); });
		}
	}
	if (!sorted_chunks[0].empty()) {
		sort_chunk(0);
	}
	for (auto &thread : threads) {
		thread.join();
	}

	struct MergeNode {
		duckdb::idx_t chunk_id = 0;
		size_t index = 0;
	};
	struct NodeGreater {
		const std::vector<std::vector<size_t>> *chunks = nullptr;
		const std::vector<Q10CandidateRow> *candidates = nullptr;
		bool operator()(const MergeNode &lhs, const MergeNode &rhs) const {
			return BetterCandidateRow((*candidates)[(*chunks)[rhs.chunk_id][rhs.index]],
			                          (*candidates)[(*chunks)[lhs.chunk_id][lhs.index]]);
		}
	};

	std::priority_queue<MergeNode, std::vector<MergeNode>, NodeGreater> heap(NodeGreater {&sorted_chunks, &candidates});
	for (duckdb::idx_t chunk_id = 0; chunk_id < workers; chunk_id++) {
		if (!sorted_chunks[chunk_id].empty()) {
			heap.push(MergeNode {chunk_id, 0});
		}
	}

	for (size_t out = 0; out < sort_order.size(); out++) {
		const auto top = heap.top();
		heap.pop();
		sort_order[out] = sorted_chunks[top.chunk_id][top.index];
		const auto next_index = top.index + 1;
		if (next_index < sorted_chunks[top.chunk_id].size()) {
			heap.push(MergeNode {top.chunk_id, next_index});
		}
	}
}

void PrintQ10ParallelProfile(const Q10ParallelProfile &profile) {
	std::cerr << "q10_parallel_profile"
	          << " nation_ns=" << profile.nation_ns << " orders_ns=" << profile.orders_ns
	          << " lineitem_ns=" << profile.lineitem_ns << " revenue_merge_ns=" << profile.revenue_merge_ns
	          << " active_customer_ns=" << profile.active_customer_ns << " customer_ns=" << profile.customer_ns
	          << " output_merge_ns=" << profile.output_merge_ns << " output_sort_ns=" << profile.output_sort_ns
	          << " output_materialize_ns=" << profile.output_materialize_ns
	          << " worker_count=" << profile.worker_count << " orders_date_filtered_rows=" << profile.orders_date_filtered_rows
	          << " lineitem_return_filtered_rows=" << profile.lineitem_return_filtered_rows
	          << " lineitem_order_matched_rows=" << profile.lineitem_order_matched_rows
	          << " lineitem_payload_rows=" << profile.lineitem_payload_rows
	          << " customer_rows_scanned=" << profile.customer_rows_scanned
	          << " customer_active_survivors=" << profile.customer_active_survivors
	          << " customer_strings_materialized=" << profile.customer_strings_materialized
	          << " active_customers=" << profile.active_customers
	          << " output_rows_before_sort=" << profile.output_rows_before_sort
	          << " orders_bitmap_bytes=" << profile.orders_bitmap_bytes << " revenue_bytes=" << profile.revenue_bytes
	          << " revenue_worker_bytes=" << profile.revenue_worker_bytes
	          << " active_customer_bitmap_bytes=" << profile.active_customer_bitmap_bytes
	          << " orders_flat_rows=" << profile.orders_flat_rows << " lineitem_flat_rows=" << profile.lineitem_flat_rows
	          << " customer_flat_rows=" << profile.customer_flat_rows
	          << " orders_fallback_rows=" << profile.orders_fallback_rows
	          << " lineitem_fallback_rows=" << profile.lineitem_fallback_rows
	          << " customer_fallback_rows=" << profile.customer_fallback_rows
	          << " min_worker_lineitem_ns=" << profile.min_worker_lineitem_ns
	          << " max_worker_lineitem_ns=" << profile.max_worker_lineitem_ns
	          << " min_worker_customer_ns=" << profile.min_worker_customer_ns
	          << " max_worker_customer_ns=" << profile.max_worker_customer_ns << "\n";
}

class Q10ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q10ParallelKernel(Q10Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 4 || !bound_inputs[0] || !bound_inputs[1] || !bound_inputs[2] ||
		    !bound_inputs[3]) {
			throw duckdb::InternalException("Q10 parallel expects bound nation, orders, lineitem, customer scans");
		}

		using clock = std::chrono::steady_clock;
		Q10ParallelProfile profile;
		const auto worker_count = ParallelWorkerCount(context);
		profile.worker_count = static_cast<size_t>(worker_count);

		const Q10ScanIndices scan_indices;

		Q10FrozenState frozen;
		const auto nation_t0 = clock::now();
		BuildNationState(context, *bound_inputs[0], frozen);
		profile.nation_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - nation_t0).count();

		const auto order_capacity = EstimateTpchDenseOrderCapacity(context, *bound_inputs[1]);
		frozen.order_customers.Preallocate(order_capacity);

		const auto orders_t0 = clock::now();
		RunParallelChunkScan(context, *bound_inputs[1], worker_count, [&](duckdb::idx_t, duckdb::DataChunk &chunk) {
			PassProfile pass_stats;
			AccumulateOrdersChunk(chunk, frozen.order_customers, pass_stats);
			if (worker_count == 1) {
				profile.orders_date_filtered_rows += pass_stats.rows_in;
				profile.orders_flat_rows += pass_stats.flat_rows;
				profile.orders_fallback_rows += pass_stats.fallback_rows;
			}
		});
		profile.orders_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - orders_t0).count();
		profile.orders_bitmap_bytes = frozen.order_customers.LogicalBytes();

		struct LineitemWorkerState {
			DenseRevenueStore revenue;
			PassProfile stats;
		};
		std::vector<CacheLinePadded<LineitemWorkerState>> lineitem_workers(static_cast<size_t>(worker_count));
		const auto lineitem_t0 = clock::now();
		RunParallelChunkScan(context, *bound_inputs[2], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     const auto chunk_t0 = clock::now();
			                     auto &worker = lineitem_workers[static_cast<size_t>(worker_id)].value;
			                     AccumulateLineitemChunk(chunk, frozen.order_customers, worker.revenue, worker.stats);
			                     worker.stats.process_ns +=
			                         std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - chunk_t0).count();
		                     });
		profile.lineitem_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - lineitem_t0).count();

		const auto merge_t0 = clock::now();
		DenseRevenueStore merged_revenue;
		std::vector<int64_t> lineitem_worker_ns(static_cast<size_t>(worker_count), 0);
		std::vector<DenseRevenueStore *> revenue_ptrs;
		revenue_ptrs.reserve(static_cast<size_t>(worker_count));
		int64_t merged_capacity = kDefaultCustomerCapacity;
		for (size_t worker_id = 0; worker_id < static_cast<size_t>(worker_count); worker_id++) {
			const auto &worker = lineitem_workers[worker_id].value;
			revenue_ptrs.push_back(const_cast<DenseRevenueStore *>(&worker.revenue));
			merged_capacity = std::max(merged_capacity, worker.revenue.Capacity());
			profile.lineitem_return_filtered_rows += worker.stats.rows_in;
			profile.lineitem_order_matched_rows += worker.stats.rows_out;
			profile.lineitem_payload_rows += worker.stats.rows_out;
			profile.lineitem_flat_rows += worker.stats.flat_rows;
			profile.lineitem_fallback_rows += worker.stats.fallback_rows;
			profile.revenue_worker_bytes += worker.revenue.LogicalBytes();
			lineitem_worker_ns[worker_id] = worker.stats.process_ns;
		}
		MergeWorkerRevenueStores(revenue_ptrs, merged_revenue, merged_capacity, worker_count);
		lineitem_workers.clear();
		profile.revenue_merge_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - merge_t0).count();
		profile.revenue_bytes = merged_revenue.LogicalBytes();
		profile.active_customers = merged_revenue.ActiveKeys().size();

		if (!lineitem_worker_ns.empty()) {
			profile.min_worker_lineitem_ns = *std::min_element(lineitem_worker_ns.begin(), lineitem_worker_ns.end());
			profile.max_worker_lineitem_ns = *std::max_element(lineitem_worker_ns.begin(), lineitem_worker_ns.end());
		}

		const auto active_t0 = clock::now();
		ActiveCustomerIndex active_customers;
		active_customers.Build(merged_revenue);
		profile.active_customer_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - active_t0).count();
		profile.active_customer_bitmap_bytes = active_customers.LogicalBytes();

		struct CustomerWorkerState {
			std::vector<Q10CandidateRow> local_candidates;
			std::vector<Q10CustomerPayload> local_payloads;
			PassProfile stats;
		};
		std::vector<CacheLinePadded<CustomerWorkerState>> customer_workers(static_cast<size_t>(worker_count));

		const auto customer_t0 = clock::now();
		if (!merged_revenue.ActiveKeys().empty()) {
			RunParallelFilteredScan(
			    context, *bound_inputs[3], worker_count,
			    [&](duckdb::idx_t worker_id, duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
			        duckdb::RowGroup &row_group, duckdb::SelectionVector &sel, duckdb::idx_t count) {
				    const auto chunk_t0 = clock::now();
				    auto &worker = customer_workers[static_cast<size_t>(worker_id)].value;
				    const auto before = worker.local_candidates.size();
				    AccumulateFusedCustomer(transaction, scan_state, row_group, sel, count, active_customers,
				                            merged_revenue, frozen, worker.local_candidates, worker.local_payloads,
				                            worker.stats);
				    worker.stats.process_ns +=
				        std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - chunk_t0).count();
				    profile.customer_strings_materialized += (worker.local_candidates.size() - before) * 4;
			    });
		}
		profile.customer_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - customer_t0).count();

		const auto output_merge_t0 = clock::now();
		std::vector<int64_t> customer_worker_ns(static_cast<size_t>(worker_count), 0);
		std::vector<Q10CandidateRow> candidates;
		std::vector<Q10CustomerPayload> payloads;
		candidates.reserve(merged_revenue.ActiveKeys().size());
		payloads.reserve(merged_revenue.ActiveKeys().size());
		for (size_t worker_id = 0; worker_id < static_cast<size_t>(worker_count); worker_id++) {
			auto &worker = customer_workers[worker_id].value;
			candidates.insert(candidates.end(), std::make_move_iterator(worker.local_candidates.begin()),
			                  std::make_move_iterator(worker.local_candidates.end()));
			payloads.insert(payloads.end(), std::make_move_iterator(worker.local_payloads.begin()),
			                std::make_move_iterator(worker.local_payloads.end()));
			profile.customer_rows_scanned += worker.stats.rows_in;
			profile.customer_active_survivors += worker.local_candidates.size();
			profile.customer_flat_rows += worker.stats.flat_rows;
			profile.customer_fallback_rows += worker.stats.fallback_rows;
			customer_worker_ns[worker_id] = worker.stats.process_ns;
		}
		customer_workers.clear();
		profile.output_merge_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - output_merge_t0).count();
		profile.output_rows_before_sort = candidates.size();

		if (!customer_worker_ns.empty()) {
			profile.min_worker_customer_ns = *std::min_element(customer_worker_ns.begin(), customer_worker_ns.end());
			profile.max_worker_customer_ns = *std::max_element(customer_worker_ns.begin(), customer_worker_ns.end());
		}

		std::vector<size_t> sort_order(candidates.size());
		std::iota(sort_order.begin(), sort_order.end(), 0);
		const auto sort_t0 = clock::now();
		ParallelSortCandidateIndices(sort_order, candidates, worker_count);
		profile.output_sort_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - sort_t0).count();
		// TPC-H Q10 returns the top 20 customers by revenue.
		constexpr std::size_t kTpchQ10ResultLimit = 20;
		if (sort_order.size() > kTpchQ10ResultLimit) {
			sort_order.resize(kTpchQ10ResultLimit);
		}

		const auto materialize_t0 = clock::now();
		output_rows.clear();
		output_rows.resize(sort_order.size());
		const auto materialize_workers = std::min<duckdb::idx_t>(worker_count, static_cast<duckdb::idx_t>(sort_order.size()));
		if (materialize_workers <= 1) {
			for (size_t row = 0; row < sort_order.size(); row++) {
				const auto index = sort_order[row];
				const auto &candidate = candidates[index];
				auto &payload = payloads[index];
				auto &out = output_rows[row];
				out.custkey = candidate.custkey;
				out.name = std::move(payload.name);
				out.revenue = candidate.revenue;
				out.acctbal = duckdb::hugeint_t(candidate.acctbal_raw);
				out.nation_name = frozen.nation_names[static_cast<size_t>(candidate.nationkey)];
				out.address = std::move(payload.address);
				out.phone = std::move(payload.phone);
				out.comment = std::move(payload.comment);
			}
		} else {
			const auto range_size = (sort_order.size() + materialize_workers - 1) / materialize_workers;
			std::vector<std::thread> threads;
			threads.reserve(materialize_workers > 0 ? materialize_workers - 1 : 0);
			std::exception_ptr error;
			std::mutex error_mutex;
			auto run_range = [&](duckdb::idx_t range_id) {
				try {
					const auto range_begin = static_cast<size_t>(range_id * range_size);
					const auto range_end = std::min(sort_order.size(), static_cast<size_t>((range_id + 1) * range_size));
					for (size_t row = range_begin; row < range_end; row++) {
						const auto index = sort_order[row];
						const auto &candidate = candidates[index];
						auto &payload = payloads[index];
						auto &out = output_rows[row];
						out.custkey = candidate.custkey;
						out.name = std::move(payload.name);
						out.revenue = candidate.revenue;
						out.acctbal = duckdb::hugeint_t(candidate.acctbal_raw);
						out.nation_name = frozen.nation_names[static_cast<size_t>(candidate.nationkey)];
						out.address = std::move(payload.address);
						out.phone = std::move(payload.phone);
						out.comment = std::move(payload.comment);
					}
				} catch (...) {
					std::lock_guard<std::mutex> lock(error_mutex);
					if (!error) {
						error = std::current_exception();
					}
				}
			};
			for (duckdb::idx_t range_id = 1; range_id < materialize_workers; range_id++) {
				threads.emplace_back([&, range_id]() { run_range(range_id); });
			}
			run_range(0);
			for (auto &thread : threads) {
				thread.join();
			}
			if (error) {
				std::rethrow_exception(error);
			}
		}
		candidates.clear();
		payloads.clear();
		profile.output_materialize_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - materialize_t0).count();

		if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
			PrintQ10ParallelProfile(profile);
		}
	}

	void Finalize() override {
		output_offset = 0;
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || output_offset >= output_rows.size()) {
			return false;
		}
		const duckdb::idx_t remaining = output_rows.size() - output_offset;
		const duckdb::idx_t count = std::min<duckdb::idx_t>(remaining, kOutputBatchSize);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto &entry = output_rows[output_offset + row];
			output.SetValue(0, row, duckdb::Value::BIGINT(entry.custkey));
			output.SetValue(1, row, duckdb::Value(entry.name));
			output.SetValue(2, row, duckdb::Value::DECIMAL(entry.revenue, kRevenueDecimalWidth, kRevenueDecimalScale));
			output.SetValue(3, row, duckdb::Value::DECIMAL(entry.acctbal, 15, 2));
			output.SetValue(4, row, duckdb::Value(entry.nation_name));
			output.SetValue(5, row, duckdb::Value(entry.address));
			output.SetValue(6, row, duckdb::Value(entry.phone));
			output.SetValue(7, row, duckdb::Value(entry.comment));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	Q10Parameters params;
	std::vector<Q10OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ10ParallelKernel(const Q10Parameters &params) {
	return duckdb::make_uniq<Q10ParallelKernel>(params);
}

} // namespace accelerator_codegen
