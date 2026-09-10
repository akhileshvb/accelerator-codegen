#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_kernel.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_runtime.hpp"
#include "tpch/q5/duckdb/base/query.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;
constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr int32_t kAbsentNation = -1;
constexpr int64_t kAbsentCustomer = 0;
constexpr int32_t kMaxNationSlots = 25;
constexpr int64_t kDefaultCustomerCapacity = 150001;
constexpr int64_t kDefaultSupplierCapacity = 10001;

#if defined(__SIZEOF_INT128__)
using q5_acc_t = __int128_t;

duckdb::hugeint_t FromAccumulator(q5_acc_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}
#else
using q5_acc_t = duckdb::hugeint_t;

duckdb::hugeint_t FromAccumulator(const q5_acc_t &value) {
	return value;
}
#endif

struct Q5OutputRow {
	std::string nation_name;
	duckdb::hugeint_t revenue;
};

bool BetterOutputRow(const Q5OutputRow &lhs, const Q5OutputRow &rhs) {
	return duckdb::Hugeint::GreaterThan(lhs.revenue, rhs.revenue);
}

bool StringEquals(const duckdb::string_t &lhs, const std::string &rhs) {
	return lhs.GetSize() == rhs.size() && std::memcmp(lhs.GetData(), rhs.data(), rhs.size()) == 0;
}

bool Q5ProfileEnabled() {
	if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
		return true;
	}
	return false;
}

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

struct Q5ParallelProfile {
	int64_t total_ns = 0;
	int64_t region_nation_ns = 0;
	int64_t customer_ns = 0;
	int64_t supplier_ns = 0;
	int64_t orders_ns = 0;
	int64_t build_ns = 0;
	int64_t lineitem_ns = 0;
	int64_t merge_ns = 0;
	int64_t output_ns = 0;
	size_t worker_count = 0;
	size_t nations_in_region = 0;
	size_t customers_scanned = 0;
	size_t customers_in_region = 0;
	size_t suppliers_scanned = 0;
	size_t suppliers_in_region = 0;
	size_t orders_scanned = 0;
	size_t orders_date_survivors = 0;
	size_t orders_customer_survivors = 0;
	size_t lineitem_rows = 0;
	size_t lineitem_order_hits = 0;
	size_t lineitem_supplier_hits = 0;
	size_t lineitem_same_nation = 0;
	size_t lineitem_revenue_contribs = 0;
	size_t lineitem_flat_rows = 0;
	size_t lineitem_fallback_rows = 0;
	size_t customer_dense_bytes = 0;
	size_t supplier_dense_bytes = 0;
	size_t order_dense_bytes = 0;
	int64_t max_lineitem_worker_ns = 0;
	int64_t avg_lineitem_worker_ns = 0;
};

void PrintQ5ParallelProfile(const Q5ParallelProfile &profile) {
	const auto pct = [&](int64_t phase_ns) {
		return profile.total_ns > 0 ? (100.0 * static_cast<double>(phase_ns) / static_cast<double>(profile.total_ns))
		                            : 0.0;
	};
	std::cerr << "q5_parallel_profile total_ns=" << profile.total_ns << " build_ns=" << profile.build_ns
	          << " (" << pct(profile.build_ns) << "%)"
	          << " region_nation_ns=" << profile.region_nation_ns << " (" << pct(profile.region_nation_ns) << "%)"
	          << " customer_ns=" << profile.customer_ns << " (" << pct(profile.customer_ns) << "%)"
	          << " supplier_ns=" << profile.supplier_ns << " (" << pct(profile.supplier_ns) << "%)"
	          << " orders_ns=" << profile.orders_ns << " (" << pct(profile.orders_ns) << "%)"
	          << " lineitem_ns=" << profile.lineitem_ns << " (" << pct(profile.lineitem_ns) << "%)"
	          << " merge_ns=" << profile.merge_ns << " (" << pct(profile.merge_ns) << "%)"
	          << " output_ns=" << profile.output_ns << " (" << pct(profile.output_ns) << "%)"
	          << " worker_count=" << profile.worker_count << " nations_in_region=" << profile.nations_in_region
	          << " customers_scanned=" << profile.customers_scanned << " customers_in_region=" << profile.customers_in_region
	          << " suppliers_scanned=" << profile.suppliers_scanned << " suppliers_in_region=" << profile.suppliers_in_region
	          << " orders_scanned=" << profile.orders_scanned << " orders_date_survivors=" << profile.orders_date_survivors
	          << " orders_customer_survivors=" << profile.orders_customer_survivors
	          << " lineitem_rows=" << profile.lineitem_rows << " lineitem_order_hits=" << profile.lineitem_order_hits
	          << " lineitem_supplier_hits=" << profile.lineitem_supplier_hits
	          << " lineitem_same_nation=" << profile.lineitem_same_nation
	          << " lineitem_revenue_contribs=" << profile.lineitem_revenue_contribs
	          << " lineitem_flat_rows=" << profile.lineitem_flat_rows
	          << " lineitem_fallback_rows=" << profile.lineitem_fallback_rows
	          << " customer_dense_bytes=" << profile.customer_dense_bytes
	          << " supplier_dense_bytes=" << profile.supplier_dense_bytes
	          << " order_dense_bytes=" << profile.order_dense_bytes
	          << " max_lineitem_worker_ns=" << profile.max_lineitem_worker_ns
	          << " avg_lineitem_worker_ns=" << profile.avg_lineitem_worker_ns << "\n";
}

template <typename ValueT, ValueT AbsentValue>
class DenseKeyStore {
public:
	DenseKeyStore() : default_capacity(0) {
	}

	explicit DenseKeyStore(int64_t default_capacity_p) : default_capacity(default_capacity_p) {
	}

	DenseKeyStore(const DenseKeyStore &) = delete;
	DenseKeyStore &operator=(const DenseKeyStore &) = delete;
	DenseKeyStore(DenseKeyStore &&) = default;
	DenseKeyStore &operator=(DenseKeyStore &&) = default;

	void WarmInitialize() {
		if (initialized) {
			return;
		}
		values.assign(static_cast<size_t>(default_capacity), AbsentValue);
		capacity = default_capacity;
		initialized = true;
	}

	void Store(int64_t key, ValueT value) {
		if (!initialized) {
			WarmInitialize();
		}
		if (key < 0) {
			overflow[key] = value;
			return;
		}
		if (key >= capacity) {
			if (key > 1'000'000'000) {
				overflow[key] = value;
				return;
			}
			EnsureCapacity(key + 1);
		}
		values[static_cast<size_t>(key)] = value;
	}

	ValueT Load(int64_t key) const {
		if (key < 0) {
			const auto it = overflow.find(key);
			return it == overflow.end() ? AbsentValue : it->second;
		}
		if (key >= 0 && key < capacity) {
			return values[static_cast<size_t>(key)];
		}
		const auto it = overflow.find(key);
		return it == overflow.end() ? AbsentValue : it->second;
	}

	bool CanProbeUnchecked(int64_t key) const {
		return key >= 0 && key < capacity;
	}

	ValueT LoadUnchecked(uint64_t key) const {
		return values[key];
	}

	void MergeFrom(const DenseKeyStore &other) {
		if (!other.initialized) {
			return;
		}
		if (!initialized) {
			WarmInitialize();
		}
		for (int64_t key = 0; key < other.capacity; key++) {
			const auto value = other.values[static_cast<size_t>(key)];
			if (value != AbsentValue) {
				Store(key, value);
			}
		}
		for (const auto &entry : other.overflow) {
			Store(entry.first, entry.second);
		}
	}

	size_t DenseBytes() const {
		return initialized ? values.size() * sizeof(ValueT) : 0;
	}

private:
	void EnsureCapacity(int64_t new_capacity) {
		if (new_capacity <= capacity) {
			return;
		}
		int64_t target = new_capacity;
		if (capacity > 0) {
			const int64_t doubled = capacity > (std::numeric_limits<int64_t>::max() / 2)
			                              ? std::numeric_limits<int64_t>::max()
			                              : capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		}
		values.resize(static_cast<size_t>(target), AbsentValue);
		capacity = target;
	}

	int64_t default_capacity;
	bool initialized = false;
	int64_t capacity = 0;
	std::vector<ValueT> values;
	std::unordered_map<int64_t, ValueT> overflow;
};

constexpr uint8_t kAbsentOrderNation = 255;

inline int32_t DecodeOrderNation(uint8_t stored_nation) {
	return stored_nation == kAbsentOrderNation ? kAbsentNation : static_cast<int32_t>(stored_nation);
}

inline int32_t LoadOrderNation(const DenseKeyStore<uint8_t, kAbsentOrderNation> &store, int64_t orderkey) {
	const uint8_t stored = store.CanProbeUnchecked(orderkey)
	                           ? store.LoadUnchecked(static_cast<uint64_t>(orderkey))
	                           : store.Load(orderkey);
	return DecodeOrderNation(stored);
}

struct Q5FrozenState {
	int32_t target_regionkey = 0;
	bool has_target_regionkey = false;
	std::array<bool, kMaxNationSlots> qualifying_nations {};
	std::array<std::string, kMaxNationSlots> nation_names;
	DenseKeyStore<int32_t, kAbsentNation> customer_nations {kDefaultCustomerCapacity};
	DenseKeyStore<int32_t, kAbsentNation> supplier_nations {kDefaultSupplierCapacity};
	DenseKeyStore<uint8_t, kAbsentOrderNation> order_nations {1};

	bool has_qualifying_nations() const {
		for (const auto flag : qualifying_nations) {
			if (flag) {
				return true;
			}
		}
		return false;
	}
};

struct Q5WorkerAgg {
	q5_acc_t revenue[kMaxNationSlots] {};
	bool has[kMaxNationSlots] {};
};

bool IsDictionaryOverFlat(duckdb::Vector &vector) {
	if (vector.GetVectorType() != duckdb::VectorType::DICTIONARY_VECTOR) {
		return false;
	}
	return duckdb::DictionaryVector::Child(vector).GetVectorType() == duckdb::VectorType::FLAT_VECTOR;
}

#if defined(__SIZEOF_INT128__)
void AddLineitemRevenue(Q5WorkerAgg &agg, int32_t nationkey, int64_t extendedprice_raw, int64_t discount_raw) {
	agg.revenue[nationkey] += static_cast<q5_acc_t>(extendedprice_raw) * static_cast<q5_acc_t>(100 - discount_raw);
	agg.has[nationkey] = true;
}
#else
void AddLineitemRevenue(Q5WorkerAgg &agg, int32_t nationkey, int64_t extendedprice_raw, int64_t discount_raw) {
	duckdb::hugeint_t price_value;
	duckdb::hugeint_t factor_value;
	duckdb::hugeint_t product_value;
	if (!duckdb::Hugeint::TryConvert(extendedprice_raw, price_value) ||
	    !duckdb::Hugeint::TryConvert(static_cast<int64_t>(100 - discount_raw), factor_value) ||
	    !duckdb::Hugeint::TryMultiply(price_value, factor_value, product_value)) {
		throw duckdb::OutOfRangeException("Overflow while computing Q5 revenue product");
	}
	if (!agg.has[nationkey]) {
		agg.revenue[nationkey] = product_value;
		agg.has[nationkey] = true;
	} else if (!duckdb::Hugeint::TryAddInPlace(agg.revenue[nationkey], product_value)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q5 revenue");
	}
}
#endif

bool ProcessLineitemRow(const Q5FrozenState &state, int64_t orderkey, int64_t suppkey, int64_t extendedprice_raw,
                        int64_t discount_raw, Q5WorkerAgg &agg, Q5ParallelProfile *profile) {
	const auto cust_nation = LoadOrderNation(state.order_nations, orderkey);
	if (cust_nation == kAbsentNation) {
		return false;
	}
	if (profile) {
		profile->lineitem_order_hits++;
	}
	const auto supp_nation = state.supplier_nations.Load(suppkey);
	if (supp_nation == kAbsentNation || supp_nation != cust_nation) {
		return false;
	}
	if (profile) {
		profile->lineitem_supplier_hits++;
		profile->lineitem_same_nation++;
	}
	if (cust_nation < 0 || cust_nation >= kMaxNationSlots) {
		return false;
	}
	AddLineitemRevenue(agg, cust_nation, extendedprice_raw, discount_raw);
	if (profile) {
		profile->lineitem_revenue_contribs++;
	}
	return true;
}

struct Q5WorkerLineitemProfile {
	int64_t elapsed_ns = 0;
	size_t rows = 0;
	size_t revenue_contribs = 0;
};

void ConsumeCustomerChunk(duckdb::DataChunk &chunk, const Q5FrozenState &state,
                          DenseKeyStore<int32_t, kAbsentNation> &customer_nations, Q5ParallelProfile *profile) {
	if (!state.has_qualifying_nations()) {
		return;
	}
	auto &custkey_vector = chunk.data[Q5CustomerChunkLayout::CUSTKEY_INDEX];
	auto &nationkey_vector = chunk.data[Q5CustomerChunkLayout::NATIONKEY_INDEX];
	const bool flat_fast_path = IsFlatAllValid(custkey_vector) && IsFlatAllValid(nationkey_vector);
	if (flat_fast_path) {
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
		auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			if (profile) {
				profile->customers_scanned++;
			}
			const auto nationkey = nationkeys[row];
			if (nationkey < 0 || nationkey >= kMaxNationSlots ||
			    !state.qualifying_nations[static_cast<size_t>(nationkey)]) {
				continue;
			}
			if (profile) {
				profile->customers_in_region++;
			}
			customer_nations.Store(custkeys[row], nationkey);
		}
		return;
	}
	duckdb::UnifiedVectorFormat custkey_format;
	duckdb::UnifiedVectorFormat nationkey_format;
	custkey_vector.ToUnifiedFormat(chunk.size(), custkey_format);
	nationkey_vector.ToUnifiedFormat(chunk.size(), nationkey_format);
	auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
	auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto custkey_index = custkey_format.sel->get_index(row);
		const auto nationkey_index = nationkey_format.sel->get_index(row);
		if ((!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
		    (!nationkey_format.validity.AllValid() && !nationkey_format.validity.RowIsValidUnsafe(nationkey_index))) {
			continue;
		}
		if (profile) {
			profile->customers_scanned++;
		}
		const auto nationkey = nationkeys[nationkey_index];
		if (nationkey < 0 || nationkey >= kMaxNationSlots ||
		    !state.qualifying_nations[static_cast<size_t>(nationkey)]) {
			continue;
		}
		if (profile) {
			profile->customers_in_region++;
		}
		customer_nations.Store(custkeys[custkey_index], nationkey);
	}
}

void ConsumeSupplierChunk(duckdb::DataChunk &chunk, const Q5FrozenState &state,
                          DenseKeyStore<int32_t, kAbsentNation> &supplier_nations, Q5ParallelProfile *profile) {
	if (!state.has_qualifying_nations()) {
		return;
	}
	auto &suppkey_vector = chunk.data[Q5SupplierChunkLayout::SUPPKEY_INDEX];
	auto &nationkey_vector = chunk.data[Q5SupplierChunkLayout::NATIONKEY_INDEX];
	const bool flat_fast_path = IsFlatAllValid(suppkey_vector) && IsFlatAllValid(nationkey_vector);
	if (flat_fast_path) {
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
		auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			if (profile) {
				profile->suppliers_scanned++;
			}
			const auto nationkey = nationkeys[row];
			if (nationkey < 0 || nationkey >= kMaxNationSlots ||
			    !state.qualifying_nations[static_cast<size_t>(nationkey)]) {
				continue;
			}
			if (profile) {
				profile->suppliers_in_region++;
			}
			supplier_nations.Store(suppkeys[row], nationkey);
		}
		return;
	}
	duckdb::UnifiedVectorFormat suppkey_format;
	duckdb::UnifiedVectorFormat nationkey_format;
	suppkey_vector.ToUnifiedFormat(chunk.size(), suppkey_format);
	nationkey_vector.ToUnifiedFormat(chunk.size(), nationkey_format);
	auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
	auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto suppkey_index = suppkey_format.sel->get_index(row);
		const auto nationkey_index = nationkey_format.sel->get_index(row);
		if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
		    (!nationkey_format.validity.AllValid() && !nationkey_format.validity.RowIsValidUnsafe(nationkey_index))) {
			continue;
		}
		if (profile) {
			profile->suppliers_scanned++;
		}
		const auto nationkey = nationkeys[nationkey_index];
		if (nationkey < 0 || nationkey >= kMaxNationSlots ||
		    !state.qualifying_nations[static_cast<size_t>(nationkey)]) {
			continue;
		}
		if (profile) {
			profile->suppliers_in_region++;
		}
		supplier_nations.Store(suppkeys[suppkey_index], nationkey);
	}
}

void ConsumeOrdersChunk(duckdb::DataChunk &chunk, const Q5Parameters &params, const Q5FrozenState &state,
                        DenseKeyStore<uint8_t, kAbsentOrderNation> &order_nations, Q5ParallelProfile *profile) {
	if (!state.has_qualifying_nations()) {
		return;
	}
	auto &orderkey_vector = chunk.data[Q5OrdersChunkLayout::ORDERKEY_INDEX];
	auto &custkey_vector = chunk.data[Q5OrdersChunkLayout::CUSTKEY_INDEX];
	auto &orderdate_vector = chunk.data[Q5OrdersChunkLayout::ORDERDATE_INDEX];
	const bool flat_fast_path =
	    IsFlatAllValid(orderkey_vector) && IsFlatAllValid(custkey_vector) && IsFlatAllValid(orderdate_vector);
	if (flat_fast_path) {
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
		auto orderdates = duckdb::FlatVector::GetData<duckdb::date_t>(orderdate_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			if (profile) {
				profile->orders_scanned++;
			}
			const auto orderdate = orderdates[row];
			if (!(orderdate >= params.start_date && orderdate < params.end_date)) {
				continue;
			}
			if (profile) {
				profile->orders_date_survivors++;
			}
			const auto custkey = custkeys[row];
			const auto nationkey = state.customer_nations.Load(custkey);
			if (nationkey == kAbsentNation) {
				continue;
			}
			if (profile) {
				profile->orders_customer_survivors++;
			}
			order_nations.Store(orderkeys[row], static_cast<uint8_t>(nationkey));
		}
		return;
	}
	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat custkey_format;
	duckdb::UnifiedVectorFormat orderdate_format;
	orderkey_vector.ToUnifiedFormat(chunk.size(), orderkey_format);
	custkey_vector.ToUnifiedFormat(chunk.size(), custkey_format);
	orderdate_vector.ToUnifiedFormat(chunk.size(), orderdate_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
	auto orderdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(orderdate_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto custkey_index = custkey_format.sel->get_index(row);
		const auto orderdate_index = orderdate_format.sel->get_index(row);
		if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
		    (!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
		    (!orderdate_format.validity.AllValid() && !orderdate_format.validity.RowIsValidUnsafe(orderdate_index))) {
			continue;
		}
		if (profile) {
			profile->orders_scanned++;
		}
		const auto orderdate = orderdates[orderdate_index];
		if (!(orderdate >= params.start_date && orderdate < params.end_date)) {
			continue;
		}
		if (profile) {
			profile->orders_date_survivors++;
		}
		const auto custkey = custkeys[custkey_index];
		const auto nationkey = state.customer_nations.Load(custkey);
		if (nationkey == kAbsentNation) {
			continue;
		}
		if (profile) {
			profile->orders_customer_survivors++;
		}
		order_nations.Store(orderkeys[orderkey_index], static_cast<uint8_t>(nationkey));
	}
}

void BuildQ5RegionNationState(duckdb::ClientContext &context, const Q5Parameters &params,
                              const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs,
                              Q5FrozenState &state) {
	auto region_source = CreateNativeChunkSource(context, *bound_inputs[0]);
	while (region_source->Next()) {
		auto &chunk = region_source->Chunk();
		duckdb::UnifiedVectorFormat regionkey_format;
		duckdb::UnifiedVectorFormat name_format;
		chunk.data[Q5RegionChunkLayout::REGIONKEY_INDEX].ToUnifiedFormat(chunk.size(), regionkey_format);
		chunk.data[Q5RegionChunkLayout::NAME_INDEX].ToUnifiedFormat(chunk.size(), name_format);
		auto regionkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(regionkey_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto regionkey_index = regionkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			if ((!regionkey_format.validity.AllValid() && !regionkey_format.validity.RowIsValidUnsafe(regionkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index))) {
				continue;
			}
			if (!StringEquals(duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index],
			                  params.region_name)) {
				continue;
			}
			state.target_regionkey = regionkeys[regionkey_index];
			state.has_target_regionkey = true;
		}
	}

	if (!state.has_target_regionkey) {
		return;
	}

	auto nation_source = CreateNativeChunkSource(context, *bound_inputs[1]);
	while (nation_source->Next()) {
		auto &chunk = nation_source->Chunk();
		duckdb::UnifiedVectorFormat nationkey_format;
		duckdb::UnifiedVectorFormat name_format;
		duckdb::UnifiedVectorFormat regionkey_format;
		chunk.data[Q5NationChunkLayout::NATIONKEY_INDEX].ToUnifiedFormat(chunk.size(), nationkey_format);
		chunk.data[Q5NationChunkLayout::NAME_INDEX].ToUnifiedFormat(chunk.size(), name_format);
		chunk.data[Q5NationChunkLayout::REGIONKEY_INDEX].ToUnifiedFormat(chunk.size(), regionkey_format);
		auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
		auto regionkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(regionkey_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto nationkey_index = nationkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			const auto regionkey_index = regionkey_format.sel->get_index(row);
			if ((!nationkey_format.validity.AllValid() && !nationkey_format.validity.RowIsValidUnsafe(nationkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index)) ||
			    (!regionkey_format.validity.AllValid() &&
			     !regionkey_format.validity.RowIsValidUnsafe(regionkey_index))) {
				continue;
			}
			if (regionkeys[regionkey_index] != state.target_regionkey) {
				continue;
			}
			const auto nationkey = nationkeys[nationkey_index];
			if (nationkey < 0 || nationkey >= kMaxNationSlots) {
				continue;
			}
			const auto name_value = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index];
			state.qualifying_nations[static_cast<size_t>(nationkey)] = true;
			state.nation_names[static_cast<size_t>(nationkey)] =
			    std::string(name_value.GetData(), name_value.GetData() + name_value.GetSize());
		}
	}

	if (!state.has_qualifying_nations()) {
		return;
	}
}

void BuildQ5FrozenState(duckdb::ClientContext &context, const Q5Parameters &params,
                        const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs,
                        Q5FrozenState &state, int64_t order_capacity, Q5ParallelProfile *profile) {
	using clock = std::chrono::steady_clock;
	const auto region_t0 = clock::now();
	BuildQ5RegionNationState(context, params, bound_inputs, state);
	if (profile) {
		profile->region_nation_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - region_t0).count();
		for (int32_t nationkey = 0; nationkey < kMaxNationSlots; nationkey++) {
			profile->nations_in_region += state.qualifying_nations[static_cast<size_t>(nationkey)] ? 1 : 0;
		}
	}
	if (!state.has_qualifying_nations()) {
		return;
	}

	state.customer_nations.WarmInitialize();
	state.supplier_nations.WarmInitialize();
	state.order_nations = DenseKeyStore<uint8_t, kAbsentOrderNation>(order_capacity);
	state.order_nations.WarmInitialize();

	{
		const auto t0 = clock::now();
		auto customer_source = CreateNativeChunkSource(context, *bound_inputs[2]);
		while (customer_source->Next()) {
			ConsumeCustomerChunk(customer_source->Chunk(), state, state.customer_nations, profile);
		}
		if (profile) {
			profile->customer_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
			profile->customer_dense_bytes = state.customer_nations.DenseBytes();
		}
	}

	{
		const auto t0 = clock::now();
		auto supplier_source = CreateNativeChunkSource(context, *bound_inputs[3]);
		while (supplier_source->Next()) {
			ConsumeSupplierChunk(supplier_source->Chunk(), state, state.supplier_nations, profile);
		}
		if (profile) {
			profile->supplier_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
			profile->supplier_dense_bytes = state.supplier_nations.DenseBytes();
		}
	}

	{
		const auto t0 = clock::now();
		auto orders_source = CreateNativeChunkSource(context, *bound_inputs[4]);
		while (orders_source->Next()) {
			ConsumeOrdersChunk(orders_source->Chunk(), params, state, state.order_nations, profile);
		}
		if (profile) {
			profile->orders_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
			profile->order_dense_bytes = state.order_nations.DenseBytes();
		}
	}
}

void AccumulateLineitemChunk(duckdb::DataChunk &chunk, const Q5FrozenState &state, Q5WorkerAgg &agg,
                             Q5ParallelProfile *profile) {
	if (!state.has_qualifying_nations()) {
		return;
	}
	if (profile) {
		profile->lineitem_rows += chunk.size();
	}
	auto &orderkey_vector = chunk.data[Q5LineitemChunkLayout::ORDERKEY_INDEX];
	auto &suppkey_vector = chunk.data[Q5LineitemChunkLayout::SUPPKEY_INDEX];
	auto &extendedprice_vector = chunk.data[Q5LineitemChunkLayout::EXTENDEDPRICE_INDEX];
	auto &discount_vector = chunk.data[Q5LineitemChunkLayout::DISCOUNT_INDEX];

	if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(suppkey_vector) && IsFlatAllValid(extendedprice_vector) &&
	    IsFlatAllValid(discount_vector)) {
		if (profile) {
			profile->lineitem_flat_rows += chunk.size();
		}
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
		auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vector);
		auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto cust_nation = LoadOrderNation(state.order_nations, orderkeys[row]);
			if (cust_nation == kAbsentNation) {
				continue;
			}
			if (profile) {
				profile->lineitem_order_hits++;
			}
			const auto supp_nation = state.supplier_nations.Load(suppkeys[row]);
			if (supp_nation == kAbsentNation || supp_nation != cust_nation) {
				continue;
			}
			if (profile) {
				profile->lineitem_supplier_hits++;
				profile->lineitem_same_nation++;
			}
			if (cust_nation < 0 || cust_nation >= kMaxNationSlots) {
				continue;
			}
			AddLineitemRevenue(agg, cust_nation, extendedprices[row], discounts[row]);
			if (profile) {
				profile->lineitem_revenue_contribs++;
			}
		}
		return;
	}

	if (IsDictionaryOverFlat(orderkey_vector) && IsDictionaryOverFlat(suppkey_vector) &&
	    IsDictionaryOverFlat(extendedprice_vector) && IsDictionaryOverFlat(discount_vector)) {
		if (profile) {
			profile->lineitem_flat_rows += chunk.size();
		}
		auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
		auto &suppkey_sel = duckdb::DictionaryVector::SelVector(suppkey_vector);
		auto &extendedprice_sel = duckdb::DictionaryVector::SelVector(extendedprice_vector);
		auto &discount_sel = duckdb::DictionaryVector::SelVector(discount_vector);
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(suppkey_vector));
		auto extendedprices =
		    duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(extendedprice_vector));
		auto discounts = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(discount_vector));
		auto &orderkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector));
		auto &suppkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(suppkey_vector));
		auto &extendedprice_validity =
		    duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(extendedprice_vector));
		auto &discount_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(discount_vector));
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto orderkey_index = orderkey_sel.get_index(row);
			const auto suppkey_index = suppkey_sel.get_index(row);
			if (!orderkey_validity.RowIsValid(orderkey_index) || !suppkey_validity.RowIsValid(suppkey_index)) {
				if (profile) {
					profile->lineitem_fallback_rows++;
				}
				continue;
			}
			const auto cust_nation = LoadOrderNation(state.order_nations, orderkeys[orderkey_index]);
			if (cust_nation == kAbsentNation) {
				continue;
			}
			if (profile) {
				profile->lineitem_order_hits++;
			}
			const auto supp_nation = state.supplier_nations.Load(suppkeys[suppkey_index]);
			if (supp_nation == kAbsentNation || supp_nation != cust_nation) {
				continue;
			}
			if (profile) {
				profile->lineitem_supplier_hits++;
				profile->lineitem_same_nation++;
			}
			const auto extendedprice_index = extendedprice_sel.get_index(row);
			const auto discount_index = discount_sel.get_index(row);
			if (!extendedprice_validity.RowIsValid(extendedprice_index) ||
			    !discount_validity.RowIsValid(discount_index)) {
				if (profile) {
					profile->lineitem_fallback_rows++;
				}
				continue;
			}
			if (cust_nation < 0 || cust_nation >= kMaxNationSlots) {
				continue;
			}
			AddLineitemRevenue(agg, cust_nation, extendedprices[extendedprice_index], discounts[discount_index]);
			if (profile) {
				profile->lineitem_revenue_contribs++;
			}
		}
		return;
	}

	if (profile) {
		profile->lineitem_fallback_rows += chunk.size();
	}
	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat suppkey_format;
	duckdb::UnifiedVectorFormat extendedprice_format;
	duckdb::UnifiedVectorFormat discount_format;
	orderkey_vector.ToUnifiedFormat(chunk.size(), orderkey_format);
	suppkey_vector.ToUnifiedFormat(chunk.size(), suppkey_format);
	extendedprice_vector.ToUnifiedFormat(chunk.size(), extendedprice_format);
	discount_vector.ToUnifiedFormat(chunk.size(), discount_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
	auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
	auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);

	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto suppkey_index = suppkey_format.sel->get_index(row);
		if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
		    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index))) {
			continue;
		}
		const auto cust_nation = LoadOrderNation(state.order_nations, orderkeys[orderkey_index]);
		if (cust_nation == kAbsentNation) {
			continue;
		}
		if (profile) {
			profile->lineitem_order_hits++;
		}
		const auto supp_nation = state.supplier_nations.Load(suppkeys[suppkey_index]);
		if (supp_nation == kAbsentNation || supp_nation != cust_nation) {
			continue;
		}
		if (profile) {
			profile->lineitem_supplier_hits++;
			profile->lineitem_same_nation++;
		}
		const auto extendedprice_index = extendedprice_format.sel->get_index(row);
		const auto discount_index = discount_format.sel->get_index(row);
		if ((!extendedprice_format.validity.AllValid() &&
		     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
		    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index))) {
			continue;
		}
		if (cust_nation < 0 || cust_nation >= kMaxNationSlots) {
			continue;
		}
		AddLineitemRevenue(agg, cust_nation, extendedprices[extendedprice_index], discounts[discount_index]);
		if (profile) {
			profile->lineitem_revenue_contribs++;
		}
	}
}

class Q5ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q5ParallelKernel(Q5Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 6) {
			throw duckdb::InternalException("Q5 parallel expects 6 bound scans");
		}

		using clock = std::chrono::steady_clock;
		const bool profile_enabled = Q5ProfileEnabled();
		Q5ParallelProfile profile;
		const auto total_t0 = clock::now();

		Q5FrozenState frozen;
		const auto order_capacity = EstimateTpchDenseOrderCapacity(context, *bound_inputs[4]);
		const auto worker_count = ParallelWorkerCount(context);
		profile.worker_count = static_cast<size_t>(worker_count);

		const auto build_t0 = clock::now();
		BuildQ5FrozenState(context, params, bound_inputs, frozen, order_capacity,
		                   profile_enabled ? &profile : nullptr);
		profile.build_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - build_t0).count();

		std::vector<CacheLinePadded<Q5WorkerAgg>> workers(worker_count);
		std::vector<Q5WorkerLineitemProfile> lineitem_worker_profiles(profile_enabled ? static_cast<size_t>(worker_count)
		                                                                              : 0);
		q5_acc_t merged_revenue[kMaxNationSlots] {};
		bool merged_has[kMaxNationSlots] {};

		if (frozen.has_qualifying_nations()) {
			const auto lineitem_t0 = clock::now();
			RunParallelChunkScan(context, *bound_inputs[5], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
				                     const auto worker_t0 = profile_enabled ? clock::now() : clock::time_point {};
				                     AccumulateLineitemChunk(chunk, frozen, workers[worker_id].value,
				                                             profile_enabled && worker_count == 1 ? &profile
				                                                                                : nullptr);
				                     if (profile_enabled) {
					                     auto &worker_profile = lineitem_worker_profiles[worker_id];
					                     worker_profile.rows += chunk.size();
					                     worker_profile.elapsed_ns +=
					                         std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() -
					                                                                            worker_t0)
					                             .count();
				                     }
			                     });
			profile.lineitem_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - lineitem_t0).count();
			if (profile_enabled) {
				int64_t worker_ns_sum = 0;
				for (const auto &worker_profile : lineitem_worker_profiles) {
					worker_ns_sum += worker_profile.elapsed_ns;
					profile.max_lineitem_worker_ns =
					    std::max(profile.max_lineitem_worker_ns, worker_profile.elapsed_ns);
				}
				profile.avg_lineitem_worker_ns =
				    lineitem_worker_profiles.empty()
				        ? 0
				        : worker_ns_sum / static_cast<int64_t>(lineitem_worker_profiles.size());
			}

			const auto merge_t0 = clock::now();
			for (const auto &worker : workers) {
				for (int32_t nationkey = 0; nationkey < kMaxNationSlots; nationkey++) {
					if (!worker.value.has[nationkey]) {
						continue;
					}
#if defined(__SIZEOF_INT128__)
					merged_revenue[nationkey] += worker.value.revenue[nationkey];
					merged_has[nationkey] = true;
#else
					if (!merged_has[nationkey]) {
						merged_revenue[nationkey] = worker.value.revenue[nationkey];
						merged_has[nationkey] = true;
					} else if (!duckdb::Hugeint::TryAddInPlace(merged_revenue[nationkey],
					                                           worker.value.revenue[nationkey])) {
						throw duckdb::OutOfRangeException("Overflow while merging Q5 revenue");
					}
#endif
				}
			}
			profile.merge_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - merge_t0).count();
		}

		{
			const auto output_t0 = clock::now();
			output_rows.clear();
			for (int32_t nationkey = 0; nationkey < kMaxNationSlots; nationkey++) {
				if (!frozen.qualifying_nations[static_cast<size_t>(nationkey)] || !merged_has[nationkey]) {
					continue;
				}
				Q5OutputRow row;
				row.nation_name = frozen.nation_names[static_cast<size_t>(nationkey)];
				row.revenue = FromAccumulator(merged_revenue[nationkey]);
				output_rows.push_back(std::move(row));
			}
			std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
			profile.output_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - output_t0).count();
		}

		profile.total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - total_t0).count();
		if (profile_enabled) {
			PrintQ5ParallelProfile(profile);
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
			output.SetValue(0, row, duckdb::Value(entry.nation_name));
			output.SetValue(1, row, duckdb::Value::DECIMAL(entry.revenue, kRevenueDecimalWidth, kRevenueDecimalScale));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	Q5Parameters params;
	std::vector<Q5OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ5ParallelKernel(const Q5Parameters &params) {
	return duckdb::make_uniq<Q5ParallelKernel>(params);
}

} // namespace accelerator_codegen
