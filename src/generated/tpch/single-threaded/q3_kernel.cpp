#include "accelerator_codegen/tpch_q3.hpp"

#include <algorithm>
#include <iostream>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;
constexpr duckdb::idx_t kTopK = 10;
constexpr duckdb::idx_t kOutputBatchSize = 10;
// TPC-H SF1 domains: custkey 1..150000, orderkey 1..6000000. Grow if exceeded.
constexpr int64_t kDefaultCustCapacity = 150001;
constexpr int64_t kDefaultOrderCapacity = 6000001;

using Clock = std::chrono::steady_clock;

#if defined(__SIZEOF_INT128__)
using q3_acc_t = __int128_t;

duckdb::hugeint_t FromAccumulator(q3_acc_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}

void AddRevenue(q3_acc_t &accumulator, int64_t extendedprice_raw, int64_t discount_raw) {
	accumulator += static_cast<q3_acc_t>(extendedprice_raw) * static_cast<q3_acc_t>(100 - discount_raw);
}

void AddAccumulated(q3_acc_t &accumulator, q3_acc_t amount) {
	accumulator += amount;
}
#else
using q3_acc_t = duckdb::hugeint_t;

duckdb::hugeint_t FromAccumulator(const q3_acc_t &value) {
	return value;
}

void AddRevenue(q3_acc_t &accumulator, int64_t extendedprice_raw, int64_t discount_raw) {
	duckdb::hugeint_t price_value;
	duckdb::hugeint_t factor_value;
	duckdb::hugeint_t product_value;
	if (!duckdb::Hugeint::TryConvert(extendedprice_raw, price_value) ||
	    !duckdb::Hugeint::TryConvert(static_cast<int64_t>(100 - discount_raw), factor_value) ||
	    !duckdb::Hugeint::TryMultiply(price_value, factor_value, product_value) ||
	    !duckdb::Hugeint::TryAddInPlace(accumulator, product_value)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q3 revenue");
	}
}

void AddAccumulated(q3_acc_t &accumulator, const q3_acc_t &amount) {
	if (!duckdb::Hugeint::TryAddInPlace(accumulator, amount)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q3 revenue");
	}
}
#endif

#ifdef ACCEL_EXPERIMENTAL_Q3_OPTS
bool ReadBoolEnv(const char *name, bool default_value) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		return default_value;
	}
	return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 || std::strcmp(value, "TRUE") == 0;
}
#endif

struct Q3RuntimeConfig {
	bool dense_customers = true;
	bool dense_orders = true;
	bool dict_fast = true;
	bool topk_heap = true;
	bool trust_native = false;
	bool shared_sel = true;
	bool run_cache = false;
	bool slot_map = true;
	bool hot_cold = false;
	bool base_residual = true;
	bool cust_partial = false;
	bool slot_absent0 = false;
	bool slot_reserve = true;
	bool orders_shared_sel = true;
};

Q3RuntimeConfig LoadQ3Config() {
	Q3RuntimeConfig config;
#ifdef ACCEL_EXPERIMENTAL_Q3_OPTS
	config.dense_customers = ReadBoolEnv("ACCEL_Q3_DENSE_CUST", true);
	config.dense_orders = ReadBoolEnv("ACCEL_Q3_DENSE_ORDERS", true);
	config.dict_fast = ReadBoolEnv("ACCEL_Q3_DICT_FAST", true);
	config.topk_heap = ReadBoolEnv("ACCEL_Q3_TOPK_HEAP", true);
	config.trust_native = ReadBoolEnv("ACCEL_Q3_TRUST_NATIVE", false);
	config.shared_sel = ReadBoolEnv("ACCEL_Q3_SHARED_SEL", true);
	config.run_cache = ReadBoolEnv("ACCEL_Q3_RUN_CACHE", false);
	config.slot_map = ReadBoolEnv("ACCEL_Q3_SLOT_MAP", true);
	config.hot_cold = ReadBoolEnv("ACCEL_Q3_HOT_COLD", false);
	config.base_residual = ReadBoolEnv("ACCEL_Q3_BASE_RESIDUAL", true);
	config.cust_partial = ReadBoolEnv("ACCEL_Q3_CUST_PARTIAL", false);
	config.slot_absent0 = ReadBoolEnv("ACCEL_Q3_SLOT_ABSENT0", false);
	config.slot_reserve = ReadBoolEnv("ACCEL_Q3_SLOT_RESERVE", true);
	config.orders_shared_sel = ReadBoolEnv("ACCEL_Q3_ORDERS_SHARED_SEL", true);
#endif
	return config;
}

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR && duckdb::FlatVector::Validity(vector).AllValid();
}

bool IsDictionaryOverFlatAllValid(duckdb::Vector &vector) {
	if (vector.GetVectorType() != duckdb::VectorType::DICTIONARY_VECTOR) {
		return false;
	}
	auto &child = duckdb::DictionaryVector::Child(vector);
	return child.GetVectorType() == duckdb::VectorType::FLAT_VECTOR && duckdb::FlatVector::Validity(child).AllValid();
}

bool IsConstantAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::CONSTANT_VECTOR && !duckdb::ConstantVector::IsNull(vector);
}

bool IsFastPathVector(duckdb::Vector &vector) {
	return IsFlatAllValid(vector) || IsDictionaryOverFlatAllValid(vector) || IsConstantAllValid(vector);
}

//! DICTIONARY vector over a FLAT child, regardless of the child's per-row validity.
bool IsDictionaryOverFlat(duckdb::Vector &vector) {
	if (vector.GetVectorType() != duckdb::VectorType::DICTIONARY_VECTOR) {
		return false;
	}
	return duckdb::DictionaryVector::Child(vector).GetVectorType() == duckdb::VectorType::FLAT_VECTOR;
}

template <class T>
const T *GetFastPathData(duckdb::Vector &vector) {
	if (vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR) {
		return duckdb::FlatVector::GetData<T>(vector);
	}
	if (vector.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR) {
		return duckdb::FlatVector::GetData<T>(duckdb::DictionaryVector::Child(vector));
	}
	return duckdb::ConstantVector::GetData<T>(vector);
}

duckdb::idx_t GetFastPathIndex(duckdb::Vector &vector, duckdb::idx_t row) {
	if (vector.GetVectorType() == duckdb::VectorType::CONSTANT_VECTOR) {
		return 0;
	}
	if (vector.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR) {
		return duckdb::DictionaryVector::SelVector(vector).get_index(row);
	}
	return row;
}

void RecordLayout(Q3LayoutHistogram *histogram, duckdb::Vector &vector) {
	if (!histogram) {
		return;
	}
	switch (vector.GetVectorType()) {
	case duckdb::VectorType::FLAT_VECTOR:
		if (duckdb::FlatVector::Validity(vector).AllValid()) {
			histogram->flat_all_valid += 1;
		} else {
			histogram->other += 1;
		}
		return;
	case duckdb::VectorType::DICTIONARY_VECTOR: {
		auto &child = duckdb::DictionaryVector::Child(vector);
		if (child.GetVectorType() == duckdb::VectorType::FLAT_VECTOR) {
			if (duckdb::FlatVector::Validity(child).AllValid()) {
				histogram->dict_flat_all_valid += 1;
			} else {
				histogram->dict_flat_nullable += 1;
			}
			return;
		}
		histogram->other += 1;
		return;
	}
	case duckdb::VectorType::CONSTANT_VECTOR:
		if (!duckdb::ConstantVector::IsNull(vector)) {
			histogram->constant_valid += 1;
		} else {
			histogram->other += 1;
		}
		return;
	default:
		histogram->other += 1;
		return;
	}
}

struct OrderState {
	duckdb::date_t orderdate;
	int32_t shippriority = 0;
	q3_acc_t revenue = {};
	bool has_revenue = false;
};

struct Q3OutputRow {
	int64_t orderkey = 0;
	duckdb::hugeint_t revenue;
	duckdb::date_t orderdate;
	int32_t shippriority = 0;
};

bool BetterRow(const Q3OutputRow &lhs, const Q3OutputRow &rhs) {
	if (lhs.revenue != rhs.revenue) {
		return lhs.revenue > rhs.revenue;
	}
	if (lhs.orderdate != rhs.orderdate) {
		return lhs.orderdate < rhs.orderdate;
	}
	if (lhs.orderkey != rhs.orderkey) {
		return lhs.orderkey < rhs.orderkey;
	}
	return lhs.shippriority < rhs.shippriority;
}

// Min-heap over the current top-k: BetterRow is "less" so top() is the worst kept row.
struct TopKLess {
	bool operator()(const Q3OutputRow &lhs, const Q3OutputRow &rhs) const {
		return BetterRow(lhs, rhs);
	}
};

class CustomerMembership {
public:
	explicit CustomerMembership(bool use_dense_p) : use_dense(use_dense_p) {
		if (use_dense) {
			EnsureCapacity(kDefaultCustCapacity);
		}
	}

	void Insert(int64_t custkey) {
		if (!use_dense || custkey < 0) {
			hash_set.insert(custkey);
			return;
		}
		if (custkey > 1'000'000'000) {
			hash_set.insert(custkey);
			return;
		}
		if (custkey >= capacity) {
			EnsureCapacity(custkey + 1);
		}
		const auto word = static_cast<uint64_t>(custkey) >> 6;
		const auto bit = uint64_t(1) << (static_cast<uint64_t>(custkey) & 63);
		bits[word] |= bit;
	}

	bool Contains(int64_t custkey) const {
		if (!use_dense || custkey < 0) {
			return hash_set.find(custkey) != hash_set.end();
		}
		if (custkey < capacity) {
			const auto word = static_cast<uint64_t>(custkey) >> 6;
			const auto bit = uint64_t(1) << (static_cast<uint64_t>(custkey) & 63);
			if ((bits[word] & bit) != 0) {
				return true;
			}
		}
		// Also probe overflow: keys may have spilled before a later dense grow.
		return hash_set.find(custkey) != hash_set.end();
	}

	duckdb::idx_t MemoryBytes() const {
		return bits.size() * sizeof(uint64_t) + hash_set.size() * (sizeof(int64_t) + sizeof(void *) * 2);
	}

	duckdb::idx_t OverflowCount() const {
		return hash_set.size();
	}

	bool UsingDense() const {
		return use_dense;
	}

private:
	void EnsureCapacity(int64_t new_capacity) {
		if (new_capacity <= capacity) {
			return;
		}
		int64_t target = new_capacity;
		if (capacity > 0) {
			const int64_t doubled = capacity > (std::numeric_limits<int64_t>::max() / 2) ? std::numeric_limits<int64_t>::max()
			                                                                             : capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		}
		const auto words = static_cast<size_t>((target + 63) / 64);
		bits.resize(words, 0);
		capacity = static_cast<int64_t>(words) * 64;
	}

	bool use_dense;
	int64_t capacity = 0;
	std::vector<uint64_t> bits;
	std::unordered_set<int64_t> hash_set;
};

// OrderStore supports three storage strategies:
//   * hash only          (use_dense == false)
//   * parallel dense     (use_dense && !use_slot_map): full-domain arrays + present/has_revenue bitmaps
//   * slot map           (use_dense && use_slot_map): orderkey->slot map + compact per-slot arrays
// Negative or absurdly large keys spill into the overflow hash_map.
// Dense capacity grows geometrically so larger scale factors need no retuning.
// Slot map absent sentinel: default -1 (memset 0xFF); ACCEL_Q3_SLOT_ABSENT0 uses 0 with stored slot+1.
class OrderStore {
public:
	OrderStore(bool use_dense_p, bool use_slot_map_p, bool slot_absent0_p = false, bool slot_reserve_p = false)
	    : use_dense(use_dense_p), use_slot_map(use_dense_p && use_slot_map_p),
	      slot_absent0(use_dense_p && use_slot_map_p && slot_absent0_p),
	      slot_reserve(use_dense_p && use_slot_map_p && slot_reserve_p) {
		// Capacity init deferred to WarmInitialize so profiling can time EnsureCapacity.
	}

	~OrderStore() {
		FreeSlotOfKey();
	}

	OrderStore(const OrderStore &) = delete;
	OrderStore &operator=(const OrderStore &) = delete;
	OrderStore(OrderStore &&) = delete;
	OrderStore &operator=(OrderStore &&) = delete;

	void SetProfile(Q3ProfileCounters *profile_p) {
		profile = profile_p;
	}

	void WarmInitialize(Q3ProfileCounters *profile_p) {
		if (profile_p) {
			profile = profile_p;
		}
		if (!use_dense) {
			return;
		}
		Clock::time_point start;
		const bool time_init = profile && use_slot_map;
		if (time_init) {
			start = Clock::now();
		}
		EnsureCapacity(kDefaultOrderCapacity);
		if (use_slot_map && slot_reserve) {
			constexpr size_t kReserve = 200000;
			slot_orderkeys.reserve(kReserve);
			slot_orderdates.reserve(kReserve);
			slot_shippriorities.reserve(kReserve);
			slot_revenues.reserve(kReserve);
			slot_has_revenue.reserve(kReserve);
			touched_slots.reserve(kReserve);
		}
		if (time_init) {
			profile->slot_map_init_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
			profile->slot_map_pages_est =
			    static_cast<duckdb::idx_t>(capacity) * sizeof(int32_t) / 4096;
		}
	}

	void UpsertOrder(int64_t orderkey, duckdb::date_t orderdate, int32_t shippriority) {
		if (!use_dense || orderkey < 0) {
			auto &state = hash_map[orderkey];
			state.orderdate = orderdate;
			state.shippriority = shippriority;
			return;
		}
		if (orderkey >= capacity) {
			// Scale-free: grow dense for normal keys; overflow only absurd outliers.
			if (orderkey > 1'000'000'000) {
				auto &state = hash_map[orderkey];
				state.orderdate = orderdate;
				state.shippriority = shippriority;
				return;
			}
			EnsureCapacity(orderkey + 1);
		}
		if (use_slot_map) {
			const auto key_idx = static_cast<size_t>(orderkey);
			const auto existing = slot_of_key[key_idx];
			if (slot_absent0) {
				if (existing != 0) {
					const auto slot = static_cast<size_t>(existing - 1);
					slot_orderdates[slot] = orderdate;
					slot_shippriorities[slot] = shippriority;
					return;
				}
				const auto new_slot = static_cast<int32_t>(slot_orderkeys.size());
				slot_of_key[key_idx] = new_slot + 1;
				slot_orderkeys.push_back(orderkey);
				slot_orderdates.push_back(orderdate);
				slot_shippriorities.push_back(shippriority);
				slot_revenues.push_back(q3_acc_t{});
				slot_has_revenue.push_back(0);
				if (profile) {
					profile->slot_allocs += 1;
				}
				return;
			}
			if (existing >= 0) {
				const auto slot = static_cast<size_t>(existing);
				slot_orderdates[slot] = orderdate;
				slot_shippriorities[slot] = shippriority;
				return;
			}
			const auto new_slot = static_cast<int32_t>(slot_orderkeys.size());
			slot_of_key[key_idx] = new_slot;
			slot_orderkeys.push_back(orderkey);
			slot_orderdates.push_back(orderdate);
			slot_shippriorities.push_back(shippriority);
			slot_revenues.push_back(q3_acc_t{});
			slot_has_revenue.push_back(0);
			if (profile) {
				profile->slot_allocs += 1;
			}
			return;
		}
		const auto idx = static_cast<size_t>(orderkey);
		const auto word = idx >> 6;
		const auto bit = uint64_t(1) << (idx & 63);
		present_bits[word] |= bit;
		orderdates[idx] = orderdate;
		shippriorities[idx] = shippriority;
	}

	OrderState *FindMutable(int64_t orderkey) {
		if (!use_dense || orderkey < 0 || orderkey >= capacity) {
			auto it = hash_map.find(orderkey);
			return it == hash_map.end() ? nullptr : &it->second;
		}
		// Dense paths use parallel/compact arrays; callers use the specialized add paths instead.
		return nullptr;
	}

	bool ContainsDense(int64_t orderkey) const {
		if (orderkey < 0 || orderkey >= capacity) {
			return false;
		}
		const auto idx = static_cast<size_t>(orderkey);
		return (present_bits[idx >> 6] & (uint64_t(1) << (idx & 63))) != 0;
	}

	void AddRevenueDense(int64_t orderkey, int64_t extendedprice_raw, int64_t discount_raw) {
		const auto idx = static_cast<size_t>(orderkey);
		const auto word = idx >> 6;
		const auto bit = uint64_t(1) << (idx & 63);
		if ((has_revenue_bits[word] & bit) == 0) {
			revenues[idx] = 0;
			has_revenue_bits[word] |= bit;
			touched.push_back(orderkey);
		}
		AddRevenue(revenues[idx], extendedprice_raw, discount_raw);
	}

	void AddRevenueHash(int64_t orderkey, int64_t extendedprice_raw, int64_t discount_raw) {
		auto it = hash_map.find(orderkey);
		if (it == hash_map.end()) {
			return;
		}
		AddRevenue(it->second.revenue, extendedprice_raw, discount_raw);
		it->second.has_revenue = true;
	}

	// Reports whether an orderkey qualifies (was inserted during the orders phase). When the slot
	// map is active and the key hit a compact slot, *out_slot is set to that slot (>= 0); otherwise
	// *out_slot is -1 and per-key add paths must be used.
	bool ProbeQualifies(int64_t orderkey, int32_t *out_slot) {
		*out_slot = -1;
		if (use_dense && orderkey >= 0 && orderkey < capacity) {
			if (use_slot_map) {
				const auto stored = slot_of_key[static_cast<size_t>(orderkey)];
				if (slot_absent0) {
					if (stored == 0) {
						return false;
					}
					*out_slot = stored - 1;
					return true;
				}
				if (stored < 0) {
					return false;
				}
				*out_slot = stored;
				return true;
			}
			return ContainsDense(orderkey);
		}
		return hash_map.find(orderkey) != hash_map.end();
	}

	void AddRevenueBySlot(int32_t slot, int64_t extendedprice_raw, int64_t discount_raw) {
		const auto s = static_cast<size_t>(slot);
		if (slot_has_revenue[s] == 0) {
			slot_revenues[s] = 0;
			slot_has_revenue[s] = 1;
			touched_slots.push_back(slot);
		}
		AddRevenue(slot_revenues[s], extendedprice_raw, discount_raw);
	}

	void AddRevenueByKey(int64_t orderkey, int64_t extendedprice_raw, int64_t discount_raw) {
		ProbeAndAddRevenue(orderkey, extendedprice_raw, discount_raw);
	}

	void AddAccumulatedBySlot(int32_t slot, q3_acc_t amount) {
		const auto s = static_cast<size_t>(slot);
		if (slot_has_revenue[s] == 0) {
			slot_revenues[s] = 0;
			slot_has_revenue[s] = 1;
			touched_slots.push_back(slot);
		}
		AddAccumulated(slot_revenues[s], amount);
	}

	void AddAccumulatedByKey(int64_t orderkey, q3_acc_t amount) {
		if (use_dense && orderkey >= 0 && orderkey < capacity) {
			if (use_slot_map) {
				const auto stored = slot_of_key[static_cast<size_t>(orderkey)];
				if (slot_absent0) {
					if (stored != 0) {
						AddAccumulatedBySlot(stored - 1, amount);
					}
					return;
				}
				if (stored >= 0) {
					AddAccumulatedBySlot(stored, amount);
				}
				return;
			}
			const auto idx = static_cast<size_t>(orderkey);
			const auto word = idx >> 6;
			const auto bit = uint64_t(1) << (idx & 63);
			if ((has_revenue_bits[word] & bit) == 0) {
				revenues[idx] = 0;
				has_revenue_bits[word] |= bit;
				touched.push_back(orderkey);
			}
			AddAccumulated(revenues[idx], amount);
			return;
		}
		auto it = hash_map.find(orderkey);
		if (it == hash_map.end()) {
			return;
		}
		AddAccumulated(it->second.revenue, amount);
		it->second.has_revenue = true;
	}

	bool ProbeAndAddRevenue(int64_t orderkey, int64_t extendedprice_raw, int64_t discount_raw) {
		if (use_dense && orderkey >= 0 && orderkey < capacity) {
			if (use_slot_map) {
				const auto stored = slot_of_key[static_cast<size_t>(orderkey)];
				if (slot_absent0) {
					if (stored == 0) {
						return false;
					}
					AddRevenueBySlot(stored - 1, extendedprice_raw, discount_raw);
					return true;
				}
				if (stored < 0) {
					return false;
				}
				AddRevenueBySlot(stored, extendedprice_raw, discount_raw);
				return true;
			}
			if (!ContainsDense(orderkey)) {
				return false;
			}
			AddRevenueDense(orderkey, extendedprice_raw, discount_raw);
			return true;
		}
		auto it = hash_map.find(orderkey);
		if (it == hash_map.end()) {
			return false;
		}
		AddRevenue(it->second.revenue, extendedprice_raw, discount_raw);
		it->second.has_revenue = true;
		return true;
	}

	template <class Emit>
	void ForEachWithRevenue(Emit &&emit) const {
		if (use_dense && use_slot_map) {
			for (const auto slot : touched_slots) {
				const auto s = static_cast<size_t>(slot);
				Q3OutputRow row;
				row.orderkey = slot_orderkeys[s];
				row.revenue = FromAccumulator(slot_revenues[s]);
				row.orderdate = slot_orderdates[s];
				row.shippriority = slot_shippriorities[s];
				emit(row);
			}
		} else if (use_dense) {
			for (const auto orderkey : touched) {
				const auto idx = static_cast<size_t>(orderkey);
				Q3OutputRow row;
				row.orderkey = orderkey;
				row.revenue = FromAccumulator(revenues[idx]);
				row.orderdate = orderdates[idx];
				row.shippriority = shippriorities[idx];
				emit(row);
			}
		}
		for (const auto &entry : hash_map) {
			if (!entry.second.has_revenue) {
				continue;
			}
			Q3OutputRow row;
			row.orderkey = entry.first;
			row.revenue = FromAccumulator(entry.second.revenue);
			row.orderdate = entry.second.orderdate;
			row.shippriority = entry.second.shippriority;
			emit(row);
		}
	}

	duckdb::idx_t TouchedCount() const {
		duckdb::idx_t count = use_slot_map ? touched_slots.size() : touched.size();
		for (const auto &entry : hash_map) {
			count += entry.second.has_revenue ? 1 : 0;
		}
		return count;
	}

	duckdb::idx_t SlotCount() const {
		return use_slot_map ? slot_orderkeys.size() : 0;
	}

	duckdb::idx_t MemoryBytes() const {
		duckdb::idx_t bytes = hash_map.size() * (sizeof(int64_t) + sizeof(OrderState) + sizeof(void *) * 2);
		if (use_slot_map) {
			if (slot_of_key) {
				bytes += static_cast<duckdb::idx_t>(capacity) * sizeof(int32_t);
			}
			bytes += slot_revenues.size() * sizeof(q3_acc_t);
			bytes += slot_orderdates.size() * sizeof(duckdb::date_t);
			bytes += slot_shippriorities.size() * sizeof(int32_t);
			bytes += slot_orderkeys.size() * sizeof(int64_t);
			bytes += slot_has_revenue.size() * sizeof(uint8_t);
			bytes += touched_slots.size() * sizeof(int32_t);
			return bytes;
		}
		bytes += present_bits.size() * sizeof(uint64_t) + has_revenue_bits.size() * sizeof(uint64_t) +
		         touched.size() * sizeof(int64_t);
		if (orderdates) {
			bytes += static_cast<duckdb::idx_t>(capacity) * sizeof(duckdb::date_t);
		}
		if (shippriorities) {
			bytes += static_cast<duckdb::idx_t>(capacity) * sizeof(int32_t);
		}
		if (revenues) {
			bytes += static_cast<duckdb::idx_t>(capacity) * sizeof(q3_acc_t);
		}
		return bytes;
	}

	duckdb::idx_t OverflowCount() const {
		return hash_map.size();
	}

	bool UsingDense() const {
		return use_dense;
	}

	bool UsingSlotMap() const {
		return use_slot_map;
	}

private:
	void FreeSlotOfKey() {
		if (!slot_of_key) {
			return;
		}
		if (slot_of_key_calloc) {
			std::free(slot_of_key);
		} else {
			delete[] slot_of_key;
		}
		slot_of_key = nullptr;
	}

	void EnsureCapacity(int64_t new_capacity) {
		if (new_capacity <= capacity) {
			return;
		}
		// Geometric growth: exact (key+1) steps across scale factors are pathological.
		int64_t target = new_capacity;
		if (capacity > 0) {
			const int64_t doubled = capacity > (std::numeric_limits<int64_t>::max() / 2) ? std::numeric_limits<int64_t>::max()
			                                                                             : capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		}
		const auto words = static_cast<size_t>((target + 63) / 64);
		const auto rounded = static_cast<int64_t>(words) * 64;

		if (use_slot_map) {
			if (slot_absent0) {
				auto *new_map =
				    static_cast<int32_t *>(std::calloc(static_cast<size_t>(rounded), sizeof(int32_t)));
				if (!new_map) {
					throw duckdb::OutOfMemoryException("Q3 slot map calloc failed");
				}
				if (capacity > 0) {
					std::memcpy(new_map, slot_of_key, static_cast<size_t>(capacity) * sizeof(int32_t));
				}
				FreeSlotOfKey();
				slot_of_key = new_map;
				slot_of_key_calloc = true;
				capacity = rounded;
				if (profile) {
					profile->slot_map_pages_est =
					    static_cast<duckdb::idx_t>(capacity) * sizeof(int32_t) / 4096;
				}
				return;
			}
			auto *new_map = new int32_t[static_cast<size_t>(rounded)];
			if (capacity > 0) {
				std::memcpy(new_map, slot_of_key, static_cast<size_t>(capacity) * sizeof(int32_t));
			}
			// Sentinel -1 for absent keys: 0xFF bytes form -1 for int32_t two's complement.
			std::memset(new_map + capacity, 0xFF, static_cast<size_t>(rounded - capacity) * sizeof(int32_t));
			FreeSlotOfKey();
			slot_of_key = new_map;
			slot_of_key_calloc = false;
			capacity = rounded;
			if (profile) {
				profile->slot_map_pages_est =
				    static_cast<duckdb::idx_t>(capacity) * sizeof(int32_t) / 4096;
			}
			return;
		}

		present_bits.resize(words, 0);
		has_revenue_bits.resize(words, 0);

		auto new_dates = std::unique_ptr<duckdb::date_t[]>(new duckdb::date_t[rounded]);
		auto new_prios = std::unique_ptr<int32_t[]>(new int32_t[rounded]);
		auto new_revs = std::unique_ptr<q3_acc_t[]>(new q3_acc_t[rounded]);
		if (capacity > 0) {
			std::memcpy(new_dates.get(), orderdates.get(), static_cast<size_t>(capacity) * sizeof(duckdb::date_t));
			std::memcpy(new_prios.get(), shippriorities.get(), static_cast<size_t>(capacity) * sizeof(int32_t));
			std::memcpy(new_revs.get(), revenues.get(), static_cast<size_t>(capacity) * sizeof(q3_acc_t));
		}
		orderdates = std::move(new_dates);
		shippriorities = std::move(new_prios);
		revenues = std::move(new_revs);
		capacity = rounded;
	}

	bool use_dense;
	bool use_slot_map;
	bool slot_absent0 = false;
	bool slot_reserve = false;
	int64_t capacity = 0;
	Q3ProfileCounters *profile = nullptr;

	// Parallel dense storage (use_slot_map == false).
	std::vector<uint64_t> present_bits;
	std::vector<uint64_t> has_revenue_bits;
	std::unique_ptr<duckdb::date_t[]> orderdates;
	std::unique_ptr<int32_t[]> shippriorities;
	std::unique_ptr<q3_acc_t[]> revenues;
	std::vector<int64_t> touched;

	// Slot map storage (use_slot_map == true).
	int32_t *slot_of_key = nullptr;
	bool slot_of_key_calloc = false;
	std::vector<q3_acc_t> slot_revenues;          // hot: written during lineitem phase
	std::vector<duckdb::date_t> slot_orderdates;  // cold: written in orders phase, read in finalize
	std::vector<int32_t> slot_shippriorities;     // cold
	std::vector<int64_t> slot_orderkeys;          // for finalize output
	std::vector<uint8_t> slot_has_revenue;
	std::vector<int32_t> touched_slots;

	// Overflow for negative / far out-of-range keys (shared by all modes).
	std::unordered_map<int64_t, OrderState> hash_map;
};

enum class Q3PathKind : uint8_t { Flat = 0, Dict = 1, Generic = 2 };

// Cross-chunk run state for the run-cache experiment. A run is a maximal sequence of
// shipdate-qualifying lineitem rows that share the same l_orderkey. The store is probed once per
// run and the run's summed revenue is written back a single time when the run is flushed.
struct LineitemRunState {
	bool active = false;
	int64_t orderkey = 0;
	bool qualifies = false;
	int32_t slot = -1; // >= 0 if slot map hit
	q3_acc_t local_revenue = {};
	duckdb::idx_t rows_in_run = 0;
};

class Q3Kernel final : public KernelInstance {
public:
	explicit Q3Kernel(Q3Parameters params_p, Q3ProfileCounters *profile_p = nullptr)
	    : params(std::move(params_p)), profile(profile_p), config(LoadQ3Config()),
	      customers(config.dense_customers),
	      orders(config.dense_orders, config.slot_map, config.slot_absent0, config.slot_reserve) {
		orders.SetProfile(profile);
		orders.WarmInitialize(profile);
		if (profile) {
			profile->used_dense_customers = config.dense_customers;
			profile->used_dense_orders = config.dense_orders;
			profile->used_topk_heap = config.topk_heap;
			profile->used_dict_fast = config.dict_fast;
			profile->used_trust_native = config.trust_native;
			profile->used_shared_sel = config.shared_sel;
			profile->used_run_cache = config.run_cache;
			profile->used_slot_map = config.slot_map;
			profile->used_hot_cold = config.hot_cold;
			profile->used_base_residual = config.base_residual;
			profile->used_cust_partial = config.cust_partial;
			profile->used_slot_absent0 = config.slot_absent0 && config.slot_map;
			profile->used_slot_reserve = config.slot_reserve && config.slot_map;
			profile->used_orders_shared_sel = config.orders_shared_sel;
		}
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		Clock::time_point start;
		if (profile) {
			start = Clock::now();
		}
		switch (static_cast<Q3InputId>(input_id)) {
		case Q3InputId::CUSTOMER:
			ConsumeCustomer(input);
			if (profile) {
				profile->customer_consume_ns +=
				    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
			}
			return;
		case Q3InputId::ORDERS:
			ConsumeOrders(input);
			if (profile) {
				profile->orders_consume_ns +=
				    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
			}
			return;
		case Q3InputId::LINEITEM:
			ConsumeLineitem(input);
			if (profile) {
				profile->lineitem_consume_ns +=
				    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
			}
			return;
		}
		throw duckdb::InternalException("Q3 kernel received unexpected input id %llu",
		                                static_cast<unsigned long long>(input_id));
	}

	void Finalize() override {
		if (finalized) {
			return;
		}
		if (config.run_cache) {
			FlushRun();
		}
		Clock::time_point start;
		if (profile) {
			start = Clock::now();
			profile->customer_state_bytes = customers.MemoryBytes();
			profile->orders_state_bytes = orders.MemoryBytes();
			profile->customer_overflow = customers.OverflowCount();
			profile->orders_overflow = orders.OverflowCount();
			profile->touched_orders = orders.TouchedCount();
			profile->slot_count = orders.SlotCount();
		}

		if (config.topk_heap) {
			std::priority_queue<Q3OutputRow, std::vector<Q3OutputRow>, TopKLess> heap;
			orders.ForEachWithRevenue([&](const Q3OutputRow &row) {
				if (heap.size() < kTopK) {
					heap.push(row);
					return;
				}
				if (BetterRow(row, heap.top())) {
					heap.pop();
					heap.push(row);
				}
			});
			ordered_rows.clear();
			ordered_rows.reserve(heap.size());
			while (!heap.empty()) {
				ordered_rows.push_back(heap.top());
				heap.pop();
			}
			std::sort(ordered_rows.begin(), ordered_rows.end(), BetterRow);
		} else {
			ordered_rows.clear();
			orders.ForEachWithRevenue([&](const Q3OutputRow &row) { ordered_rows.push_back(row); });
			std::sort(ordered_rows.begin(), ordered_rows.end(), BetterRow);
			if (ordered_rows.size() > kTopK) {
				ordered_rows.resize(kTopK);
			}
		}

		if (profile) {
			profile->aggregated_orders = ordered_rows.size();
			profile->finalize_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
		}
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || output_offset >= ordered_rows.size()) {
			return false;
		}

		const auto remaining = ordered_rows.size() - output_offset;
		const auto count = std::min<duckdb::idx_t>(remaining, kOutputBatchSize);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto &result = ordered_rows[output_offset + row];
			output.SetValue(0, row, duckdb::Value::BIGINT(result.orderkey));
			output.SetValue(1, row, duckdb::Value::DECIMAL(result.revenue, kRevenueDecimalWidth, kRevenueDecimalScale));
			output.SetValue(2, row, duckdb::Value::DATE(result.orderdate));
			output.SetValue(3, row, duckdb::Value::INTEGER(result.shippriority));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	static void TrackPath(Q3InputProfile *input_profile, duckdb::DataChunk &input, Q3PathKind kind) {
		if (!input_profile) {
			return;
		}
		input_profile->total_chunks += 1;
		input_profile->total_rows += input.size();
		switch (kind) {
		case Q3PathKind::Flat:
			input_profile->flat_fast_path_chunks += 1;
			input_profile->flat_fast_path_rows += input.size();
			break;
		case Q3PathKind::Dict:
			input_profile->dict_fast_path_chunks += 1;
			input_profile->dict_fast_path_rows += input.size();
			break;
		case Q3PathKind::Generic:
			input_profile->generic_fallback_chunks += 1;
			input_profile->generic_fallback_rows += input.size();
			break;
		}
	}

	// Decides, for a whole chunk, whether the residual date comparison can be skipped. The native
	// scan already applies the date filter; with BASE_RESIDUAL we trust that for DICTIONARY vectors
	// (base-storage filtered) but keep the residual check for FLAT/other vectors. TRUST_NATIVE skips
	// unconditionally (retained, off by default).
	bool SkipDateResidual(duckdb::Vector &date_vector, duckdb::idx_t rows) {
		if (config.trust_native) {
			return true;
		}
		if (config.base_residual) {
			if (date_vector.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR) {
				if (profile) {
					profile->residual_date_skips += rows;
				}
				return true;
			}
			if (profile) {
				profile->residual_date_checks += rows;
			}
		}
		return false;
	}

	void ProcessLineitemRow(int64_t orderkey, int64_t extendedprice_raw, int64_t discount_raw) {
		if (!config.run_cache) {
			if (orders.ProbeAndAddRevenue(orderkey, extendedprice_raw, discount_raw)) {
				if (profile) {
					profile->qualifying_lineitems += 1;
					profile->aggregate_writes += 1;
				}
			}
			return;
		}

		if (run_state.active && run_state.orderkey == orderkey) {
			if (run_state.qualifies) {
				AddRevenue(run_state.local_revenue, extendedprice_raw, discount_raw);
				if (profile) {
					profile->qualifying_lineitems += 1;
				}
			}
			run_state.rows_in_run += 1;
			if (profile) {
				profile->order_probes_avoided += 1;
			}
			return;
		}

		FlushRun();

		int32_t slot = -1;
		const bool qualifies = orders.ProbeQualifies(orderkey, &slot);
		if (profile) {
			profile->order_probes += 1;
		}
		run_state.active = true;
		run_state.orderkey = orderkey;
		run_state.qualifies = qualifies;
		run_state.slot = slot;
		run_state.local_revenue = q3_acc_t{};
		run_state.rows_in_run = 1;
		if (qualifies) {
			AddRevenue(run_state.local_revenue, extendedprice_raw, discount_raw);
			if (profile) {
				profile->qualifying_lineitems += 1;
			}
		}
	}

	void FlushRun() {
		if (!run_state.active) {
			return;
		}
		if (profile) {
			profile->order_runs += 1;
			profile->order_run_rows += run_state.rows_in_run;
		}
		if (run_state.qualifies && run_state.rows_in_run > 0) {
			if (run_state.slot >= 0) {
				orders.AddAccumulatedBySlot(run_state.slot, run_state.local_revenue);
			} else {
				orders.AddAccumulatedByKey(run_state.orderkey, run_state.local_revenue);
			}
			if (profile) {
				profile->aggregate_writes += 1;
			}
		}
		run_state.active = false;
		run_state.qualifies = false;
		run_state.slot = -1;
		run_state.local_revenue = q3_acc_t{};
		run_state.rows_in_run = 0;
	}

	void ConsumeCustomer(duckdb::DataChunk &input) {
		if (profile) {
			RecordLayout(&profile->layout_c_mktsegment, input.data[Q3CustomerChunkLayout::MKTSEGMENT_INDEX]);
			RecordLayout(&profile->layout_c_custkey, input.data[Q3CustomerChunkLayout::CUSTKEY_INDEX]);
		}
		if (Q3CustomerChunkCanUseFlatFastPath(input)) {
			TrackPath(profile ? &profile->customer : nullptr, input, Q3PathKind::Flat);
			ConsumeCustomerFast(input);
			return;
		}
		if (config.dict_fast && Q3CustomerChunkCanUseDictFastPath(input)) {
			TrackPath(profile ? &profile->customer : nullptr, input, Q3PathKind::Dict);
			ConsumeCustomerFast(input);
			return;
		}
		if (config.cust_partial && Q3CustomerChunkCanUsePartialDictFastPath(input)) {
			TrackPath(profile ? &profile->customer : nullptr, input, Q3PathKind::Dict);
			ConsumeCustomerPartial(input);
			return;
		}
		TrackPath(profile ? &profile->customer : nullptr, input, Q3PathKind::Generic);
		ConsumeCustomerGeneric(input);
	}

	void ConsumeOrders(duckdb::DataChunk &input) {
		if (profile) {
			RecordLayout(&profile->layout_o_orderdate, input.data[Q3OrdersChunkLayout::ORDERDATE_INDEX]);
			RecordLayout(&profile->layout_o_orderkey, input.data[Q3OrdersChunkLayout::ORDERKEY_INDEX]);
			RecordLayout(&profile->layout_o_custkey, input.data[Q3OrdersChunkLayout::CUSTKEY_INDEX]);
			RecordLayout(&profile->layout_o_shippriority, input.data[Q3OrdersChunkLayout::SHIPPRIORITY_INDEX]);
		}
		if (Q3OrdersChunkCanUseFlatFastPath(input)) {
			TrackPath(profile ? &profile->orders : nullptr, input, Q3PathKind::Flat);
			ConsumeOrdersFast(input);
			return;
		}
		if (config.dict_fast && Q3OrdersChunkCanUseDictFastPath(input)) {
			TrackPath(profile ? &profile->orders : nullptr, input, Q3PathKind::Dict);
			ConsumeOrdersFast(input);
			return;
		}
		TrackPath(profile ? &profile->orders : nullptr, input, Q3PathKind::Generic);
		ConsumeOrdersGeneric(input);
	}

	void ConsumeLineitem(duckdb::DataChunk &input) {
		if (profile) {
			RecordLayout(&profile->layout_l_shipdate, input.data[Q3LineitemChunkLayout::SHIPDATE_INDEX]);
			RecordLayout(&profile->layout_l_orderkey, input.data[Q3LineitemChunkLayout::ORDERKEY_INDEX]);
			RecordLayout(&profile->layout_l_extendedprice, input.data[Q3LineitemChunkLayout::EXTENDEDPRICE_INDEX]);
			RecordLayout(&profile->layout_l_discount, input.data[Q3LineitemChunkLayout::DISCOUNT_INDEX]);
		}
		if (Q3LineitemChunkCanUseFlatFastPath(input)) {
			TrackPath(profile ? &profile->lineitem : nullptr, input, Q3PathKind::Flat);
			ConsumeLineitemFast(input);
			return;
		}
		if (config.dict_fast && Q3LineitemChunkCanUseDictFastPath(input)) {
			TrackPath(profile ? &profile->lineitem : nullptr, input, Q3PathKind::Dict);
			ConsumeLineitemFast(input);
			return;
		}
		TrackPath(profile ? &profile->lineitem : nullptr, input, Q3PathKind::Generic);
		ConsumeLineitemGeneric(input);
	}

	void ConsumeCustomerFast(duckdb::DataChunk &input) {
		auto &segment_vector = input.data[Q3CustomerChunkLayout::MKTSEGMENT_INDEX];
		auto &custkey_vector = input.data[Q3CustomerChunkLayout::CUSTKEY_INDEX];
		auto segments = GetFastPathData<duckdb::string_t>(segment_vector);
		auto custkeys = GetFastPathData<int64_t>(custkey_vector);
		const bool skip_segment = config.trust_native;
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto custkey_index = GetFastPathIndex(custkey_vector, row);
			if (!skip_segment) {
				const auto segment_index = GetFastPathIndex(segment_vector, row);
				if (segments[segment_index].GetString() != params.market_segment) {
					continue;
				}
			}
			customers.Insert(custkeys[custkey_index]);
			if (profile) {
				profile->qualifying_customers += 1;
			}
		}
	}

	// CUST_PARTIAL fast path: custkey is a fast-path vector and mktsegment is a DICTIONARY over a
	// FLAT child whose per-row validity may be partial. We use fast indexing but consult the child
	// validity mask for the selected index when it is not all-valid.
	void ConsumeCustomerPartial(duckdb::DataChunk &input) {
		auto &segment_vector = input.data[Q3CustomerChunkLayout::MKTSEGMENT_INDEX];
		auto &custkey_vector = input.data[Q3CustomerChunkLayout::CUSTKEY_INDEX];
		auto &segment_child = duckdb::DictionaryVector::Child(segment_vector);
		auto segments = duckdb::FlatVector::GetData<duckdb::string_t>(segment_child);
		auto &segment_validity = duckdb::FlatVector::Validity(segment_child);
		const bool segment_all_valid = segment_validity.AllValid();
		auto custkeys = GetFastPathData<int64_t>(custkey_vector);
		const bool skip_segment = config.trust_native;
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto custkey_index = GetFastPathIndex(custkey_vector, row);
			if (!skip_segment) {
				const auto segment_index = GetFastPathIndex(segment_vector, row);
				if (!segment_all_valid && !segment_validity.RowIsValidUnsafe(segment_index)) {
					continue;
				}
				if (segments[segment_index].GetString() != params.market_segment) {
					continue;
				}
			}
			customers.Insert(custkeys[custkey_index]);
			if (profile) {
				profile->qualifying_customers += 1;
			}
		}
	}

	void ConsumeCustomerGeneric(duckdb::DataChunk &input) {
		duckdb::UnifiedVectorFormat segment_format;
		duckdb::UnifiedVectorFormat custkey_format;
		auto &segment_vector = input.data[Q3CustomerChunkLayout::MKTSEGMENT_INDEX];
		auto &custkey_vector = input.data[Q3CustomerChunkLayout::CUSTKEY_INDEX];
		segment_vector.ToUnifiedFormat(input.size(), segment_format);
		custkey_vector.ToUnifiedFormat(input.size(), custkey_format);

		auto segments = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(segment_format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto segment_index = segment_format.sel->get_index(row);
			const auto custkey_index = custkey_format.sel->get_index(row);
			if ((!segment_format.validity.AllValid() && !segment_format.validity.RowIsValidUnsafe(segment_index)) ||
			    (!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index))) {
				continue;
			}
			if (!config.trust_native && segments[segment_index].GetString() != params.market_segment) {
				continue;
			}
			customers.Insert(custkeys[custkey_index]);
			if (profile) {
				profile->qualifying_customers += 1;
			}
		}
	}

	void ConsumeOrdersFast(duckdb::DataChunk &input) {
		auto &orderdate_vector = input.data[Q3OrdersChunkLayout::ORDERDATE_INDEX];
		auto &orderkey_vector = input.data[Q3OrdersChunkLayout::ORDERKEY_INDEX];
		auto &custkey_vector = input.data[Q3OrdersChunkLayout::CUSTKEY_INDEX];
		auto &shippriority_vector = input.data[Q3OrdersChunkLayout::SHIPPRIORITY_INDEX];
		auto orderdates = GetFastPathData<duckdb::date_t>(orderdate_vector);
		auto orderkeys = GetFastPathData<int64_t>(orderkey_vector);
		auto custkeys = GetFastPathData<int64_t>(custkey_vector);
		auto shippriorities = GetFastPathData<int32_t>(shippriority_vector);
		const bool skip_date = SkipDateResidual(orderdate_vector, input.size());

		// Shared selection across the three DICT columns (shippriority is typically CONSTANT).
		const duckdb::SelectionVector *shared_sel = nullptr;
		const bool ship_const = shippriority_vector.GetVectorType() == duckdb::VectorType::CONSTANT_VECTOR;
		if (config.orders_shared_sel && orderdate_vector.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR &&
		    orderkey_vector.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR &&
		    custkey_vector.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR) {
			auto &sel0 = duckdb::DictionaryVector::SelVector(orderdate_vector);
			auto &sel1 = duckdb::DictionaryVector::SelVector(orderkey_vector);
			auto &sel2 = duckdb::DictionaryVector::SelVector(custkey_vector);
			if (sel0.data() == sel1.data() && sel1.data() == sel2.data()) {
				shared_sel = &sel0;
				if (profile) {
					profile->orders_shared_sel_chunks += 1;
				}
			}
		}

		Clock::time_point upsert_start;
		if (profile) {
			upsert_start = Clock::now();
		}
		duckdb::idx_t local_qual = 0;

		if (shared_sel) {
			const int32_t ship_const_val = ship_const ? shippriorities[0] : 0;
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto idx = shared_sel->get_index(row);
				if (!skip_date && !(orderdates[idx] < params.cutoff_date)) {
					continue;
				}
				if (!customers.Contains(custkeys[idx])) {
					continue;
				}
				const auto shippriority =
				    ship_const ? ship_const_val
				               : shippriorities[GetFastPathIndex(shippriority_vector, row)];
				orders.UpsertOrder(orderkeys[idx], orderdates[idx], shippriority);
				local_qual += 1;
			}
		} else {
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto orderdate_index = GetFastPathIndex(orderdate_vector, row);
				const auto orderkey_index = GetFastPathIndex(orderkey_vector, row);
				const auto custkey_index = GetFastPathIndex(custkey_vector, row);
				const auto shippriority_index = GetFastPathIndex(shippriority_vector, row);
				if (!skip_date && !(orderdates[orderdate_index] < params.cutoff_date)) {
					continue;
				}
				if (!customers.Contains(custkeys[custkey_index])) {
					continue;
				}
				orders.UpsertOrder(orderkeys[orderkey_index], orderdates[orderdate_index],
				                   shippriorities[shippriority_index]);
				local_qual += 1;
			}
		}

		if (profile) {
			profile->orders_upsert_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - upsert_start).count();
			profile->qualifying_orders += local_qual;
		}
	}

	void ConsumeOrdersGeneric(duckdb::DataChunk &input) {
		duckdb::UnifiedVectorFormat orderdate_format;
		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat custkey_format;
		duckdb::UnifiedVectorFormat shippriority_format;

		auto &orderdate_vector = input.data[Q3OrdersChunkLayout::ORDERDATE_INDEX];
		auto &orderkey_vector = input.data[Q3OrdersChunkLayout::ORDERKEY_INDEX];
		auto &custkey_vector = input.data[Q3OrdersChunkLayout::CUSTKEY_INDEX];
		auto &shippriority_vector = input.data[Q3OrdersChunkLayout::SHIPPRIORITY_INDEX];
		orderdate_vector.ToUnifiedFormat(input.size(), orderdate_format);
		orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
		custkey_vector.ToUnifiedFormat(input.size(), custkey_format);
		shippriority_vector.ToUnifiedFormat(input.size(), shippriority_format);

		auto orderdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(orderdate_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
		auto shippriorities = duckdb::UnifiedVectorFormat::GetData<int32_t>(shippriority_format);

		const bool skip_date = SkipDateResidual(orderdate_vector, input.size());

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto orderdate_index = orderdate_format.sel->get_index(row);
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			const auto custkey_index = custkey_format.sel->get_index(row);
			const auto shippriority_index = shippriority_format.sel->get_index(row);
			if ((!orderdate_format.validity.AllValid() && !orderdate_format.validity.RowIsValidUnsafe(orderdate_index)) ||
			    (!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
			    (!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
			    (!shippriority_format.validity.AllValid() &&
			     !shippriority_format.validity.RowIsValidUnsafe(shippriority_index))) {
				continue;
			}
			if (!skip_date && !(orderdates[orderdate_index] < params.cutoff_date)) {
				continue;
			}
			if (!customers.Contains(custkeys[custkey_index])) {
				continue;
			}
			if (profile) {
				const auto upsert_start = Clock::now();
				orders.UpsertOrder(orderkeys[orderkey_index], orderdates[orderdate_index],
				                   shippriorities[shippriority_index]);
				profile->orders_upsert_ns +=
				    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - upsert_start).count();
				profile->qualifying_orders += 1;
			} else {
				orders.UpsertOrder(orderkeys[orderkey_index], orderdates[orderdate_index],
				                   shippriorities[shippriority_index]);
			}
		}
	}

	void ConsumeLineitemFast(duckdb::DataChunk &input) {
		auto &shipdate_vector = input.data[Q3LineitemChunkLayout::SHIPDATE_INDEX];
		auto &orderkey_vector = input.data[Q3LineitemChunkLayout::ORDERKEY_INDEX];
		auto &extendedprice_vector = input.data[Q3LineitemChunkLayout::EXTENDEDPRICE_INDEX];
		auto &discount_vector = input.data[Q3LineitemChunkLayout::DISCOUNT_INDEX];
		auto shipdates = GetFastPathData<duckdb::date_t>(shipdate_vector);
		auto orderkeys = GetFastPathData<int64_t>(orderkey_vector);
		auto extendedprices = GetFastPathData<int64_t>(extendedprice_vector);
		auto discounts = GetFastPathData<int64_t>(discount_vector);
		const bool skip_shipdate = SkipDateResidual(shipdate_vector, input.size());

		const duckdb::SelectionVector *shared_sel = nullptr;
		if (config.shared_sel && shipdate_vector.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR &&
		    orderkey_vector.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR &&
		    extendedprice_vector.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR &&
		    discount_vector.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR) {
			auto &sel0 = duckdb::DictionaryVector::SelVector(shipdate_vector);
			auto &sel1 = duckdb::DictionaryVector::SelVector(orderkey_vector);
			auto &sel2 = duckdb::DictionaryVector::SelVector(extendedprice_vector);
			auto &sel3 = duckdb::DictionaryVector::SelVector(discount_vector);
			if (sel0.data() == sel1.data() && sel1.data() == sel2.data() && sel2.data() == sel3.data()) {
				shared_sel = &sel0;
				if (profile) {
					profile->shared_sel_chunks += 1;
				}
			}
		}

		if (shared_sel) {
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto idx = shared_sel->get_index(row);
				if (!skip_shipdate && !(shipdates[idx] > params.cutoff_date)) {
					continue;
				}
				ProcessLineitemRow(orderkeys[idx], extendedprices[idx], discounts[idx]);
			}
			return;
		}

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto shipdate_index = GetFastPathIndex(shipdate_vector, row);
			const auto orderkey_index = GetFastPathIndex(orderkey_vector, row);
			const auto extendedprice_index = GetFastPathIndex(extendedprice_vector, row);
			const auto discount_index = GetFastPathIndex(discount_vector, row);
			if (!skip_shipdate && !(shipdates[shipdate_index] > params.cutoff_date)) {
				continue;
			}
			ProcessLineitemRow(orderkeys[orderkey_index], extendedprices[extendedprice_index],
			                   discounts[discount_index]);
		}
	}

	void ConsumeLineitemGeneric(duckdb::DataChunk &input) {
		duckdb::UnifiedVectorFormat shipdate_format;
		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat extendedprice_format;
		duckdb::UnifiedVectorFormat discount_format;

		auto &shipdate_vector = input.data[Q3LineitemChunkLayout::SHIPDATE_INDEX];
		auto &orderkey_vector = input.data[Q3LineitemChunkLayout::ORDERKEY_INDEX];
		auto &extendedprice_vector = input.data[Q3LineitemChunkLayout::EXTENDEDPRICE_INDEX];
		auto &discount_vector = input.data[Q3LineitemChunkLayout::DISCOUNT_INDEX];
		shipdate_vector.ToUnifiedFormat(input.size(), shipdate_format);
		orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
		extendedprice_vector.ToUnifiedFormat(input.size(), extendedprice_format);
		discount_vector.ToUnifiedFormat(input.size(), discount_format);

		auto shipdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdate_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
		auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);

		const bool skip_shipdate = SkipDateResidual(shipdate_vector, input.size());

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto shipdate_index = shipdate_format.sel->get_index(row);
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			const auto extendedprice_index = extendedprice_format.sel->get_index(row);
			const auto discount_index = discount_format.sel->get_index(row);
			if ((!shipdate_format.validity.AllValid() && !shipdate_format.validity.RowIsValidUnsafe(shipdate_index)) ||
			    (!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
			    (!extendedprice_format.validity.AllValid() &&
			     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
			    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index))) {
				continue;
			}
			if (!skip_shipdate && !(shipdates[shipdate_index] > params.cutoff_date)) {
				continue;
			}
			ProcessLineitemRow(orderkeys[orderkey_index], extendedprices[extendedprice_index],
			                   discounts[discount_index]);
		}
	}

	Q3Parameters params;
	Q3ProfileCounters *profile;
	Q3RuntimeConfig config;
	CustomerMembership customers;
	OrderStore orders;
	LineitemRunState run_state;
	std::vector<Q3OutputRow> ordered_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

bool ChunkAllFastPath(duckdb::DataChunk &input, std::initializer_list<duckdb::idx_t> indexes) {
	for (const auto index : indexes) {
		if (!IsFastPathVector(input.data[index])) {
			return false;
		}
	}
	return true;
}

bool ChunkAllFlat(duckdb::DataChunk &input, std::initializer_list<duckdb::idx_t> indexes) {
	for (const auto index : indexes) {
		if (!IsFlatAllValid(input.data[index])) {
			return false;
		}
	}
	return true;
}

bool ChunkAllDict(duckdb::DataChunk &input, std::initializer_list<duckdb::idx_t> indexes) {
	for (const auto index : indexes) {
		if (!IsDictionaryOverFlatAllValid(input.data[index])) {
			return false;
		}
	}
	return true;
}

} // namespace

bool Q3CustomerChunkCanUseFlatFastPath(duckdb::DataChunk &input) {
	return ChunkAllFlat(input, {Q3CustomerChunkLayout::MKTSEGMENT_INDEX, Q3CustomerChunkLayout::CUSTKEY_INDEX});
}

bool Q3OrdersChunkCanUseFlatFastPath(duckdb::DataChunk &input) {
	return ChunkAllFlat(input, {Q3OrdersChunkLayout::ORDERDATE_INDEX, Q3OrdersChunkLayout::ORDERKEY_INDEX,
	                            Q3OrdersChunkLayout::CUSTKEY_INDEX, Q3OrdersChunkLayout::SHIPPRIORITY_INDEX});
}

bool Q3LineitemChunkCanUseFlatFastPath(duckdb::DataChunk &input) {
	return ChunkAllFlat(input, {Q3LineitemChunkLayout::SHIPDATE_INDEX, Q3LineitemChunkLayout::ORDERKEY_INDEX,
	                            Q3LineitemChunkLayout::EXTENDEDPRICE_INDEX, Q3LineitemChunkLayout::DISCOUNT_INDEX});
}

bool Q3CustomerChunkCanUseDictFastPath(duckdb::DataChunk &input) {
	return ChunkAllDict(input, {Q3CustomerChunkLayout::MKTSEGMENT_INDEX, Q3CustomerChunkLayout::CUSTKEY_INDEX}) ||
	       ChunkAllFastPath(input, {Q3CustomerChunkLayout::MKTSEGMENT_INDEX, Q3CustomerChunkLayout::CUSTKEY_INDEX});
}

bool Q3OrdersChunkCanUseDictFastPath(duckdb::DataChunk &input) {
	return ChunkAllFastPath(input, {Q3OrdersChunkLayout::ORDERDATE_INDEX, Q3OrdersChunkLayout::ORDERKEY_INDEX,
	                                Q3OrdersChunkLayout::CUSTKEY_INDEX, Q3OrdersChunkLayout::SHIPPRIORITY_INDEX});
}

bool Q3LineitemChunkCanUseDictFastPath(duckdb::DataChunk &input) {
	return ChunkAllFastPath(input, {Q3LineitemChunkLayout::SHIPDATE_INDEX, Q3LineitemChunkLayout::ORDERKEY_INDEX,
	                                Q3LineitemChunkLayout::EXTENDEDPRICE_INDEX, Q3LineitemChunkLayout::DISCOUNT_INDEX});
}

// CUST_PARTIAL eligibility: custkey is a fast-path vector (flat all-valid, dict-over-flat all-valid,
// or constant) and mktsegment is a DICTIONARY over a FLAT child (which may carry per-row nulls).
bool Q3CustomerChunkCanUsePartialDictFastPath(duckdb::DataChunk &input) {
	return IsDictionaryOverFlat(input.data[Q3CustomerChunkLayout::MKTSEGMENT_INDEX]) &&
	       IsFastPathVector(input.data[Q3CustomerChunkLayout::CUSTKEY_INDEX]);
}

duckdb::unique_ptr<KernelInstance> CreateQ3Kernel(const Q3Parameters &params) {
	return duckdb::make_uniq<Q3Kernel>(params);
}

duckdb::unique_ptr<KernelInstance> CreateQ3ProfilingKernel(const Q3Parameters &params, Q3ProfileCounters &counters) {
	return duckdb::make_uniq<Q3Kernel>(params, &counters);
}

} // namespace accelerator_codegen
