#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_kernel.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_runtime.hpp"
#include "tpch/q7/duckdb/base/query.hpp"
#include "tpch/q7/duckdb/base/q7_fast.hpp"

#include <algorithm>
#include <array>
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
constexpr uint8_t kAbsentNationClass = 0;
constexpr int64_t kDefaultCustomerCapacity = 150001;
constexpr int64_t kDefaultSupplierCapacity = 10001;
constexpr int32_t kMaxNationSlots = 25;
constexpr int64_t kMinYear = 1995;
constexpr int64_t kMaxYear = 1996;
constexpr int kMaxGroupSlots = 4;

#if defined(__SIZEOF_INT128__)
using q7_acc_t = __int128_t;

duckdb::hugeint_t FromAccumulator(q7_acc_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}
#else
using q7_acc_t = duckdb::hugeint_t;

duckdb::hugeint_t FromAccumulator(const q7_acc_t &value) {
	return value;
}
#endif

enum class NationClass : uint8_t {
	Absent = 0,
	Nation1 = 1,
	Nation2 = 2,
};

struct Q7OutputRow {
	std::string supp_nation;
	std::string cust_nation;
	int64_t year = 0;
	duckdb::hugeint_t revenue;
};

struct GroupSlotMeta {
	std::string supp_nation;
	std::string cust_nation;
	int64_t year = 0;
};

bool BetterOutputRow(const Q7OutputRow &lhs, const Q7OutputRow &rhs) {
	if (lhs.supp_nation != rhs.supp_nation) {
		return lhs.supp_nation < rhs.supp_nation;
	}
	if (lhs.cust_nation != rhs.cust_nation) {
		return lhs.cust_nation < rhs.cust_nation;
	}
	return lhs.year < rhs.year;
}

bool StringEquals(const duckdb::string_t &lhs, const std::string &rhs) {
	return lhs.GetSize() == rhs.size() && std::memcmp(lhs.GetData(), rhs.data(), rhs.size()) == 0;
}

bool Q7ProfileEnabled() {
	if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
		return true;
	}
	return false;
}

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

bool IsDictionaryOverFlat(duckdb::Vector &vector) {
	if (vector.GetVectorType() != duckdb::VectorType::DICTIONARY_VECTOR) {
		return false;
	}
	return duckdb::DictionaryVector::Child(vector).GetVectorType() == duckdb::VectorType::FLAT_VECTOR;
}

struct Q7ParallelProfile {
	int64_t total_ns = 0;
	int64_t nation_ns = 0;
	int64_t customer_ns = 0;
	int64_t supplier_ns = 0;
	int64_t orders_ns = 0;
	int64_t build_ns = 0;
	int64_t lineitem_ns = 0;
	int64_t merge_ns = 0;
	int64_t output_ns = 0;
	size_t worker_count = 0;
	size_t nations_scanned = 0;
	size_t nations_qualified = 0;
	size_t customers_scanned = 0;
	size_t customers_qualified = 0;
	size_t suppliers_scanned = 0;
	size_t suppliers_qualified = 0;
	size_t orders_scanned = 0;
	size_t orders_qualified = 0;
	size_t lineitem_rows = 0;
	size_t lineitem_order_hits = 0;
	size_t lineitem_supp_hits = 0;
	size_t lineitem_cross_nation = 0;
	size_t lineitem_shipdate_hits = 0;
	size_t lineitem_revenue_contribs = 0;
	size_t lineitem_flat_rows = 0;
	size_t lineitem_dict_rows = 0;
	size_t lineitem_fallback_rows = 0;
	size_t customer_dense_bytes = 0;
	size_t supplier_dense_bytes = 0;
	size_t order_dense_bytes = 0;
	int64_t max_lineitem_worker_ns = 0;
	int64_t avg_lineitem_worker_ns = 0;
};

void PrintQ7ParallelProfile(const Q7ParallelProfile &profile) {
	const auto pct = [&](int64_t phase_ns) {
		return profile.total_ns > 0 ? (100.0 * static_cast<double>(phase_ns) / static_cast<double>(profile.total_ns))
		                            : 0.0;
	};
	std::cerr << "q7_parallel_profile total_ns=" << profile.total_ns << " build_ns=" << profile.build_ns << " ("
	          << pct(profile.build_ns) << "%)"
	          << " nation_ns=" << profile.nation_ns << " (" << pct(profile.nation_ns) << "%)"
	          << " customer_ns=" << profile.customer_ns << " (" << pct(profile.customer_ns) << "%)"
	          << " supplier_ns=" << profile.supplier_ns << " (" << pct(profile.supplier_ns) << "%)"
	          << " orders_ns=" << profile.orders_ns << " (" << pct(profile.orders_ns) << "%)"
	          << " lineitem_ns=" << profile.lineitem_ns << " (" << pct(profile.lineitem_ns) << "%)"
	          << " merge_ns=" << profile.merge_ns << " (" << pct(profile.merge_ns) << "%)"
	          << " output_ns=" << profile.output_ns << " (" << pct(profile.output_ns) << "%)"
	          << " worker_count=" << profile.worker_count << " nations_scanned=" << profile.nations_scanned
	          << " nations_qualified=" << profile.nations_qualified << " customers_scanned=" << profile.customers_scanned
	          << " customers_qualified=" << profile.customers_qualified
	          << " suppliers_scanned=" << profile.suppliers_scanned
	          << " suppliers_qualified=" << profile.suppliers_qualified << " orders_scanned=" << profile.orders_scanned
	          << " orders_qualified=" << profile.orders_qualified << " lineitem_rows=" << profile.lineitem_rows
	          << " lineitem_order_hits=" << profile.lineitem_order_hits
	          << " lineitem_supp_hits=" << profile.lineitem_supp_hits
	          << " lineitem_cross_nation=" << profile.lineitem_cross_nation
	          << " lineitem_shipdate_hits=" << profile.lineitem_shipdate_hits
	          << " lineitem_revenue_contribs=" << profile.lineitem_revenue_contribs
	          << " lineitem_flat_rows=" << profile.lineitem_flat_rows
	          << " lineitem_dict_rows=" << profile.lineitem_dict_rows
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

	const ValueT *Data() const {
		return values.data();
	}

	int64_t Capacity() const {
		return capacity;
	}

	//! Store into the dense array without growing; false when the key lies outside it.  Safe to call
	//! concurrently for distinct keys.
	bool TryStoreDense(int64_t key, ValueT value) {
		if (static_cast<uint64_t>(key) < static_cast<uint64_t>(capacity)) {
			values[static_cast<size_t>(key)] = value;
			return true;
		}
		return false;
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

struct Q7FrozenState {
	std::array<NationClass, kMaxNationSlots> nation_class {};
	DenseKeyStore<uint8_t, kAbsentNationClass> customer_class {kDefaultCustomerCapacity};
	DenseKeyStore<uint8_t, kAbsentNationClass> supplier_class {kDefaultSupplierCapacity};
	DenseKeyStore<uint8_t, kAbsentNationClass> order_cust_class {1};
	q7fast::ByteBitmap order_bitmap;
	duckdb::date_t shipdate_start {};
	duckdb::date_t shipdate_end {};
	duckdb::date_t year_1996_start {};

	bool has_qualifying_nations() const {
		for (const auto cls : nation_class) {
			if (cls != NationClass::Absent) {
				return true;
			}
		}
		return false;
	}

	int ComputeGroupSlot(NationClass supp_class, NationClass cust_class, int64_t year) const {
		if (year < kMinYear || year > kMaxYear) {
			return -1;
		}
		int direction = -1;
		if (supp_class == NationClass::Nation1 && cust_class == NationClass::Nation2) {
			direction = 0;
		} else if (supp_class == NationClass::Nation2 && cust_class == NationClass::Nation1) {
			direction = 1;
		} else {
			return -1;
		}
		return direction * 2 + static_cast<int>(year - kMinYear);
	}
};

struct Q7WorkerAgg {
	q7_acc_t revenue[kMaxGroupSlots] {};
	bool has[kMaxGroupSlots] {};
};

struct Q7WorkerLineitemProfile {
	int64_t elapsed_ns = 0;
	size_t rows = 0;
	size_t revenue_contribs = 0;
};

#if defined(__SIZEOF_INT128__)
void AddRevenue(Q7WorkerAgg &agg, int slot, int64_t extendedprice_raw, int64_t discount_raw) {
	agg.revenue[slot] += static_cast<q7_acc_t>(extendedprice_raw) * static_cast<q7_acc_t>(100 - discount_raw);
	agg.has[slot] = true;
}
#else
void AddRevenue(Q7WorkerAgg &agg, int slot, int64_t extendedprice_raw, int64_t discount_raw) {
	duckdb::hugeint_t price_value;
	duckdb::hugeint_t factor_value;
	duckdb::hugeint_t product_value;
	if (!duckdb::Hugeint::TryConvert(extendedprice_raw, price_value) ||
	    !duckdb::Hugeint::TryConvert(static_cast<int64_t>(100 - discount_raw), factor_value) ||
	    !duckdb::Hugeint::TryMultiply(price_value, factor_value, product_value)) {
		throw duckdb::OutOfRangeException("Overflow while computing Q7 revenue product");
	}
	if (!agg.has[slot]) {
		agg.revenue[slot] = product_value;
		agg.has[slot] = true;
	} else if (!duckdb::Hugeint::TryAddInPlace(agg.revenue[slot], product_value)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q7 revenue");
	}
}
#endif

int ProbeLineitemSurvivor(const Q7FrozenState &state, int64_t orderkey, int64_t suppkey, duckdb::date_t shipdate,
                          Q7ParallelProfile *profile) {
	if (shipdate < state.shipdate_start || shipdate > state.shipdate_end) {
		return -1;
	}
	const auto supp_class_raw = state.supplier_class.CanProbeUnchecked(suppkey)
	                                ? state.supplier_class.LoadUnchecked(static_cast<uint64_t>(suppkey))
	                                : state.supplier_class.Load(suppkey);
	if (supp_class_raw == kAbsentNationClass) {
		return -1;
	}
	if (profile) {
		profile->lineitem_supp_hits++;
	}
	const auto cust_class_raw = state.order_cust_class.CanProbeUnchecked(orderkey)
	                                ? state.order_cust_class.LoadUnchecked(static_cast<uint64_t>(orderkey))
	                                : state.order_cust_class.Load(orderkey);
	if (cust_class_raw == kAbsentNationClass) {
		return -1;
	}
	if (profile) {
		profile->lineitem_order_hits++;
	}
	const auto cust_class = static_cast<NationClass>(cust_class_raw);
	const auto supp_class = static_cast<NationClass>(supp_class_raw);
	int64_t year = kMinYear;
	if (shipdate >= state.year_1996_start) {
		year = kMaxYear;
	}
	const int slot = state.ComputeGroupSlot(supp_class, cust_class, year);
	if (slot < 0) {
		return -1;
	}
	if (profile) {
		profile->lineitem_cross_nation++;
	}
	if (profile) {
		profile->lineitem_shipdate_hits++;
	}
	return slot;
}

void ConsumeNationChunk(duckdb::DataChunk &chunk, const Q7Parameters &params, Q7FrozenState &state,
                        Q7ParallelProfile *profile) {
	auto &nationkey_vector = chunk.data[Q7NationChunkLayout::NATIONKEY_INDEX];
	auto &name_vector = chunk.data[Q7NationChunkLayout::NAME_INDEX];
	if (IsFlatAllValid(nationkey_vector) && IsFlatAllValid(name_vector)) {
		auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
		auto names = duckdb::FlatVector::GetData<duckdb::string_t>(name_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			if (profile) {
				profile->nations_scanned++;
			}
			const auto nationkey = nationkeys[row];
			if (nationkey < 0 || nationkey >= kMaxNationSlots) {
				continue;
			}
			if (StringEquals(names[row], params.nation1)) {
				state.nation_class[static_cast<size_t>(nationkey)] = NationClass::Nation1;
				if (profile) {
					profile->nations_qualified++;
				}
			} else if (StringEquals(names[row], params.nation2)) {
				state.nation_class[static_cast<size_t>(nationkey)] = NationClass::Nation2;
				if (profile) {
					profile->nations_qualified++;
				}
			}
		}
		return;
	}
	duckdb::UnifiedVectorFormat nationkey_format;
	duckdb::UnifiedVectorFormat name_format;
	nationkey_vector.ToUnifiedFormat(chunk.size(), nationkey_format);
	name_vector.ToUnifiedFormat(chunk.size(), name_format);
	auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto nationkey_index = nationkey_format.sel->get_index(row);
		const auto name_index = name_format.sel->get_index(row);
		if ((!nationkey_format.validity.AllValid() && !nationkey_format.validity.RowIsValidUnsafe(nationkey_index)) ||
		    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index))) {
			continue;
		}
		if (profile) {
			profile->nations_scanned++;
		}
		const auto nationkey = nationkeys[nationkey_index];
		if (nationkey < 0 || nationkey >= kMaxNationSlots) {
			continue;
		}
		const auto name_value = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index];
		if (StringEquals(name_value, params.nation1)) {
			state.nation_class[static_cast<size_t>(nationkey)] = NationClass::Nation1;
			if (profile) {
				profile->nations_qualified++;
			}
		} else if (StringEquals(name_value, params.nation2)) {
			state.nation_class[static_cast<size_t>(nationkey)] = NationClass::Nation2;
			if (profile) {
				profile->nations_qualified++;
			}
		}
	}
}

void ConsumeCustomerChunk(duckdb::DataChunk &chunk, const Q7FrozenState &state,
                          DenseKeyStore<uint8_t, kAbsentNationClass> &customer_class, Q7ParallelProfile *profile) {
	if (!state.has_qualifying_nations()) {
		return;
	}
	auto &custkey_vector = chunk.data[Q7CustomerChunkLayout::CUSTKEY_INDEX];
	auto &nationkey_vector = chunk.data[Q7CustomerChunkLayout::NATIONKEY_INDEX];
	if (IsFlatAllValid(custkey_vector) && IsFlatAllValid(nationkey_vector)) {
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
		auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			if (profile) {
				profile->customers_scanned++;
			}
			const auto nationkey = nationkeys[row];
			if (nationkey < 0 || nationkey >= kMaxNationSlots) {
				continue;
			}
			const auto nation_cls = state.nation_class[static_cast<size_t>(nationkey)];
			if (nation_cls == NationClass::Absent) {
				continue;
			}
			if (profile) {
				profile->customers_qualified++;
			}
			customer_class.Store(custkeys[row], static_cast<uint8_t>(nation_cls));
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
		if (nationkey < 0 || nationkey >= kMaxNationSlots) {
			continue;
		}
		const auto nation_cls = state.nation_class[static_cast<size_t>(nationkey)];
		if (nation_cls == NationClass::Absent) {
			continue;
		}
		if (profile) {
			profile->customers_qualified++;
		}
		customer_class.Store(custkeys[custkey_index], static_cast<uint8_t>(nation_cls));
	}
}

void ConsumeSupplierChunk(duckdb::DataChunk &chunk, const Q7FrozenState &state,
                          DenseKeyStore<uint8_t, kAbsentNationClass> &supplier_class, Q7ParallelProfile *profile) {
	if (!state.has_qualifying_nations()) {
		return;
	}
	auto &suppkey_vector = chunk.data[Q7SupplierChunkLayout::SUPPKEY_INDEX];
	auto &nationkey_vector = chunk.data[Q7SupplierChunkLayout::NATIONKEY_INDEX];
	if (IsFlatAllValid(suppkey_vector) && IsFlatAllValid(nationkey_vector)) {
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
		auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			if (profile) {
				profile->suppliers_scanned++;
			}
			const auto nationkey = nationkeys[row];
			if (nationkey < 0 || nationkey >= kMaxNationSlots) {
				continue;
			}
			const auto nation_cls = state.nation_class[static_cast<size_t>(nationkey)];
			if (nation_cls == NationClass::Absent) {
				continue;
			}
			if (profile) {
				profile->suppliers_qualified++;
			}
			supplier_class.Store(suppkeys[row], static_cast<uint8_t>(nation_cls));
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
		if (nationkey < 0 || nationkey >= kMaxNationSlots) {
			continue;
		}
		const auto nation_cls = state.nation_class[static_cast<size_t>(nationkey)];
		if (nation_cls == NationClass::Absent) {
			continue;
		}
		if (profile) {
			profile->suppliers_qualified++;
		}
		supplier_class.Store(suppkeys[suppkey_index], static_cast<uint8_t>(nation_cls));
	}
}

template <class StoreFn>
void ConsumeOrdersChunk(duckdb::DataChunk &chunk, const Q7FrozenState &state, StoreFn &&store_order,
                        Q7ParallelProfile *profile) {
	if (!state.has_qualifying_nations()) {
		return;
	}
	auto &orderkey_vector = chunk.data[Q7OrdersChunkLayout::ORDERKEY_INDEX];
	auto &custkey_vector = chunk.data[Q7OrdersChunkLayout::CUSTKEY_INDEX];
	if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(custkey_vector)) {
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			if (profile) {
				profile->orders_scanned++;
			}
			const auto cust_class = state.customer_class.Load(custkeys[row]);
			if (cust_class == kAbsentNationClass) {
				continue;
			}
			if (profile) {
				profile->orders_qualified++;
			}
			store_order(orderkeys[row], cust_class);
		}
		return;
	}
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
		if (profile) {
			profile->orders_scanned++;
		}
		const auto cust_class = state.customer_class.Load(custkeys[custkey_index]);
		if (cust_class == kAbsentNationClass) {
			continue;
		}
		if (profile) {
			profile->orders_qualified++;
		}
		store_order(orderkeys[orderkey_index], cust_class);
	}
}

void BuildQ7FrozenState(duckdb::ClientContext &context, const Q7Parameters &params,
                        const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs,
                        Q7FrozenState &state, int64_t order_capacity, duckdb::idx_t build_worker_count,
                        Q7ParallelProfile *profile) {
	using clock = std::chrono::steady_clock;
	state.shipdate_start = params.shipdate_start;
	state.shipdate_end = params.shipdate_end;
	state.year_1996_start = duckdb::Date::FromString("1996-01-01");

	{
		const auto t0 = clock::now();
		auto nation_source = CreateNativeChunkSource(context, *bound_inputs[0]);
		while (nation_source->Next()) {
			ConsumeNationChunk(nation_source->Chunk(), params, state, profile);
		}
		if (profile) {
			profile->nation_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
		}
	}

	if (!state.has_qualifying_nations()) {
		return;
	}

	state.customer_class.WarmInitialize();
	state.supplier_class.WarmInitialize();
	state.order_cust_class = DenseKeyStore<uint8_t, kAbsentNationClass>(order_capacity);
	state.order_cust_class.WarmInitialize();

	{
		const auto t0 = clock::now();
		auto customer_source = CreateNativeChunkSource(context, *bound_inputs[1]);
		while (customer_source->Next()) {
			ConsumeCustomerChunk(customer_source->Chunk(), state, state.customer_class, profile);
		}
		if (profile) {
			profile->customer_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
			profile->customer_dense_bytes = state.customer_class.DenseBytes();
		}
	}

	{
		const auto t0 = clock::now();
		auto supplier_source = CreateNativeChunkSource(context, *bound_inputs[2]);
		while (supplier_source->Next()) {
			ConsumeSupplierChunk(supplier_source->Chunk(), state, state.supplier_class, profile);
		}
		if (profile) {
			profile->supplier_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
			profile->supplier_dense_bytes = state.supplier_class.DenseBytes();
		}
	}

	{
		const auto t0 = clock::now();
		Q7ParallelProfile *orders_profile = build_worker_count == 1 ? profile : nullptr;
		std::vector<CacheLinePadded<std::vector<std::pair<int64_t, uint8_t>>>> extra(build_worker_count);
		RunParallelChunkScan(context, *bound_inputs[3], build_worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     auto &local_extra = extra[worker_id].value;
			                     ConsumeOrdersChunk(
			                         chunk, state,
			                         [&](int64_t orderkey, uint8_t cls) {
				                         if (!state.order_cust_class.TryStoreDense(orderkey, cls)) {
					                         local_extra.emplace_back(orderkey, cls);
				                         }
			                         },
			                         orders_profile);
		                     });
		for (const auto &local_extra : extra) {
			for (const auto &entry : local_extra.value) {
				state.order_cust_class.Store(entry.first, entry.second);
			}
		}
		state.order_bitmap.Build(state.order_cust_class.Data(),
		                         static_cast<size_t>(state.order_cust_class.Capacity()), kAbsentNationClass);
		if (profile) {
			profile->orders_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
			profile->order_dense_bytes = state.order_cust_class.DenseBytes();
		}
	}
}

void AccumulateLineitemChunk(duckdb::DataChunk &chunk, const Q7FrozenState &state, Q7WorkerAgg &agg,
                             Q7ParallelProfile *profile) {
	if (!state.has_qualifying_nations()) {
		return;
	}
	if (profile) {
		profile->lineitem_rows += chunk.size();
	}
	auto &orderkey_vector = chunk.data[Q7LineitemChunkLayout::ORDERKEY_INDEX];
	auto &suppkey_vector = chunk.data[Q7LineitemChunkLayout::SUPPKEY_INDEX];
	auto &shipdate_vector = chunk.data[Q7LineitemChunkLayout::SHIPDATE_INDEX];
	auto &extendedprice_vector = chunk.data[Q7LineitemChunkLayout::EXTENDEDPRICE_INDEX];
	auto &discount_vector = chunk.data[Q7LineitemChunkLayout::DISCOUNT_INDEX];

	if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(suppkey_vector) && IsFlatAllValid(shipdate_vector) &&
	    IsFlatAllValid(extendedprice_vector) && IsFlatAllValid(discount_vector)) {
		if (profile) {
			profile->lineitem_flat_rows += chunk.size();
		}
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
		auto shipdates = duckdb::FlatVector::GetData<duckdb::date_t>(shipdate_vector);
		auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vector);
		auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vector);
		const duckdb::idx_t total = chunk.size();
		for (duckdb::idx_t off = 0; off < total; off += q7fast::kBlock) {
			const auto cnt = std::min<duckdb::idx_t>(q7fast::kBlock, total - off);
			alignas(64) uint32_t sel[q7fast::kBlock + simd::kSelSlack];
			const auto m = q7fast::SelectInBitmap(
			    orderkeys + off, cnt, state.order_bitmap.Words(), state.order_bitmap.Bits(), sel,
			    [&](int64_t key) { return state.order_cust_class.Load(key) != kAbsentNationClass; });
			for (size_t j = 0; j < m; j++) {
				const auto row = off + sel[j];
				const int slot = ProbeLineitemSurvivor(state, orderkeys[row], suppkeys[row], shipdates[row], profile);
				if (slot < 0) {
					continue;
				}
				AddRevenue(agg, slot, extendedprices[row], discounts[row]);
				if (profile) {
					profile->lineitem_revenue_contribs++;
				}
			}
		}
		return;
	}

	if (IsDictionaryOverFlat(orderkey_vector) && IsDictionaryOverFlat(suppkey_vector) &&
	    IsDictionaryOverFlat(shipdate_vector) && IsDictionaryOverFlat(extendedprice_vector) &&
	    IsDictionaryOverFlat(discount_vector)) {
		if (profile) {
			profile->lineitem_dict_rows += chunk.size();
		}
		auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
		auto &suppkey_sel = duckdb::DictionaryVector::SelVector(suppkey_vector);
		auto &shipdate_sel = duckdb::DictionaryVector::SelVector(shipdate_vector);
		auto &extendedprice_sel = duckdb::DictionaryVector::SelVector(extendedprice_vector);
		auto &discount_sel = duckdb::DictionaryVector::SelVector(discount_vector);
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(suppkey_vector));
		auto shipdates = duckdb::FlatVector::GetData<duckdb::date_t>(duckdb::DictionaryVector::Child(shipdate_vector));
		auto extendedprices =
		    duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(extendedprice_vector));
		auto discounts = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(discount_vector));
		auto &orderkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector));
		auto &suppkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(suppkey_vector));
		auto &shipdate_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(shipdate_vector));
		auto &extendedprice_validity =
		    duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(extendedprice_vector));
		auto &discount_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(discount_vector));
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto orderkey_index = orderkey_sel.get_index(row);
			const auto suppkey_index = suppkey_sel.get_index(row);
			const auto shipdate_index = shipdate_sel.get_index(row);
			if (!orderkey_validity.RowIsValid(orderkey_index) || !suppkey_validity.RowIsValid(suppkey_index) ||
			    !shipdate_validity.RowIsValid(shipdate_index)) {
				if (profile) {
					profile->lineitem_fallback_rows++;
				}
				continue;
			}
			const int slot =
			    ProbeLineitemSurvivor(state, orderkeys[orderkey_index], suppkeys[suppkey_index], shipdates[shipdate_index],
			                          profile);
			if (slot < 0) {
				continue;
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
			AddRevenue(agg, slot, extendedprices[extendedprice_index], discounts[discount_index]);
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
	duckdb::UnifiedVectorFormat shipdate_format;
	duckdb::UnifiedVectorFormat extendedprice_format;
	duckdb::UnifiedVectorFormat discount_format;
	orderkey_vector.ToUnifiedFormat(chunk.size(), orderkey_format);
	suppkey_vector.ToUnifiedFormat(chunk.size(), suppkey_format);
	shipdate_vector.ToUnifiedFormat(chunk.size(), shipdate_format);
	extendedprice_vector.ToUnifiedFormat(chunk.size(), extendedprice_format);
	discount_vector.ToUnifiedFormat(chunk.size(), discount_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
	auto shipdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdate_format);
	auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
	auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);

	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto suppkey_index = suppkey_format.sel->get_index(row);
		const auto shipdate_index = shipdate_format.sel->get_index(row);
		if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
		    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
		    (!shipdate_format.validity.AllValid() && !shipdate_format.validity.RowIsValidUnsafe(shipdate_index))) {
			continue;
		}
		const int slot = ProbeLineitemSurvivor(state, orderkeys[orderkey_index], suppkeys[suppkey_index],
		                                      shipdates[shipdate_index], profile);
		if (slot < 0) {
			continue;
		}
		const auto extendedprice_index = extendedprice_format.sel->get_index(row);
		const auto discount_index = discount_format.sel->get_index(row);
		if ((!extendedprice_format.validity.AllValid() &&
		     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
		    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index))) {
			continue;
		}
		AddRevenue(agg, slot, extendedprices[extendedprice_index], discounts[discount_index]);
		if (profile) {
			profile->lineitem_revenue_contribs++;
		}
	}
}

GroupSlotMeta MetaForSlot(int slot, const Q7Parameters &params) {
	GroupSlotMeta meta;
	const int direction = slot / 2;
	meta.year = kMinYear + (slot % 2);
	if (direction == 0) {
		meta.supp_nation = params.nation1;
		meta.cust_nation = params.nation2;
	} else {
		meta.supp_nation = params.nation2;
		meta.cust_nation = params.nation1;
	}
	return meta;
}

class Q7ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q7ParallelKernel(Q7Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 5) {
			throw duckdb::InternalException("Q7 parallel expects 5 bound scans");
		}

		using clock = std::chrono::steady_clock;
		const bool profile_enabled = Q7ProfileEnabled();
		Q7ParallelProfile profile;
		const auto total_t0 = clock::now();

		Q7FrozenState frozen;
		const auto order_capacity = EstimateTpchDenseOrderCapacity(context, *bound_inputs[3]);
		const auto worker_count = ParallelWorkerCount(context);
		profile.worker_count = static_cast<size_t>(worker_count);

		const auto build_t0 = clock::now();
		BuildQ7FrozenState(context, params, bound_inputs, frozen, order_capacity, worker_count,
		                   profile_enabled ? &profile : nullptr);
		profile.build_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - build_t0).count();

		std::vector<CacheLinePadded<Q7WorkerAgg>> workers(worker_count);
		std::vector<Q7WorkerLineitemProfile> lineitem_worker_profiles(profile_enabled ? static_cast<size_t>(worker_count)
		                                                                              : 0);
		q7_acc_t merged_revenue[kMaxGroupSlots] {};
		bool merged_has[kMaxGroupSlots] {};

		if (frozen.has_qualifying_nations()) {
			const auto lineitem_t0 = clock::now();
			RunParallelChunkScan(context, *bound_inputs[4], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
				                     const auto worker_t0 = profile_enabled ? clock::now() : clock::time_point {};
				                     AccumulateLineitemChunk(chunk, frozen, workers[worker_id].value,
				                                             profile_enabled && worker_count == 1 ? &profile : nullptr);
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
				for (int slot = 0; slot < kMaxGroupSlots; slot++) {
					if (!worker.value.has[slot]) {
						continue;
					}
#if defined(__SIZEOF_INT128__)
					merged_revenue[slot] += worker.value.revenue[slot];
					merged_has[slot] = true;
#else
					if (!merged_has[slot]) {
						merged_revenue[slot] = worker.value.revenue[slot];
						merged_has[slot] = true;
					} else if (!duckdb::Hugeint::TryAddInPlace(merged_revenue[slot], worker.value.revenue[slot])) {
						throw duckdb::OutOfRangeException("Overflow while merging Q7 revenue");
					}
#endif
				}
			}
			profile.merge_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - merge_t0).count();
		}

		{
			const auto output_t0 = clock::now();
			output_rows.clear();
			for (int slot = 0; slot < kMaxGroupSlots; slot++) {
				if (!merged_has[slot]) {
					continue;
				}
				const auto meta = MetaForSlot(slot, params);
				Q7OutputRow row;
				row.supp_nation = meta.supp_nation;
				row.cust_nation = meta.cust_nation;
				row.year = meta.year;
				row.revenue = FromAccumulator(merged_revenue[slot]);
				output_rows.push_back(std::move(row));
			}
			std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
			profile.output_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - output_t0).count();
		}

		profile.total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - total_t0).count();
		if (profile_enabled) {
			PrintQ7ParallelProfile(profile);
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
			output.SetValue(0, row, duckdb::Value(entry.supp_nation));
			output.SetValue(1, row, duckdb::Value(entry.cust_nation));
			output.SetValue(2, row, duckdb::Value::BIGINT(entry.year));
			output.SetValue(3, row, duckdb::Value::DECIMAL(entry.revenue, kRevenueDecimalWidth, kRevenueDecimalScale));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	Q7Parameters params;
	std::vector<Q7OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ7ParallelKernel(const Q7Parameters &params) {
	return duckdb::make_uniq<Q7ParallelKernel>(params);
}

} // namespace accelerator_codegen
