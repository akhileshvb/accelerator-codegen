#include "tpch/q3/duckdb/base/bitmap_select.hpp"
#include "tpch/q3/duckdb/base/query.hpp"

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

	//! Positions i in [0, n) whose key keys[idx ? idx[i] : i] is a member (dense bitmap only; the
	//! caller must check HasOverflow() == false first).
	size_t SelectMembers(const int64_t *keys, const uint32_t *idx, size_t n, uint32_t *out) const {
		return q3simd::SelectBits(keys, idx, n, bits.data(), static_cast<uint64_t>(capacity), out);
	}

	bool HasOverflow() const {
		return !hash_set.empty();
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

// OrderStore: qualifying orders are kept as a dense presence bitmap over the orderkey domain
// (750 KB at SF1, L2 resident) plus, once the orders phase is complete (Seal), compact per-order
// arrays indexed by the key's rank in the bitmap (prefix popcounts).  Lineitem probes first test
// the bitmap -- a cheap, cache-resident filter that rejects most rows -- and only qualifying rows
// compute a rank and touch the revenue arrays.  Negative or absurdly large keys spill into the
// overflow hash_map.  Dense capacity grows geometrically so larger scale factors need no retuning.
class OrderStore {
public:
	explicit OrderStore(bool use_dense_p) : use_dense(use_dense_p) {
	}

	void SetProfile(Q3ProfileCounters *profile_p) {
		profile = profile_p;
	}

	void WarmInitialize(Q3ProfileCounters *profile_p) {
		if (profile_p) {
			profile = profile_p;
		}
		if (use_dense) {
			EnsureCapacity(kDefaultOrderCapacity);
		}
	}

	void UpsertOrder(int64_t orderkey, duckdb::date_t orderdate, int32_t shippriority) {
		if (!use_dense || orderkey < 0 || orderkey > 1'000'000'000) {
			auto &state = hash_map[orderkey];
			state.orderdate = orderdate;
			state.shippriority = shippriority;
			return;
		}
		if (sealed) {
			throw duckdb::InternalException("Q3 orders arrived after lineitem started");
		}
		if (orderkey >= capacity) {
			EnsureCapacity(orderkey + 1);
		}
		const auto idx = static_cast<size_t>(orderkey);
		present_bits[idx >> 6] |= uint64_t(1) << (idx & 63);
		pending.push_back(PendingOrder {orderkey, orderdate, shippriority});
	}

	//! Build rank prefixes and the compact per-order arrays.  Called once, before the first lineitem row.
	void Seal() {
		if (sealed) {
			return;
		}
		sealed = true;
		rank_prefix.resize(present_bits.size() + 1);
		uint32_t running = 0;
		for (size_t w = 0; w < present_bits.size(); w++) {
			rank_prefix[w] = running;
			running += static_cast<uint32_t>(__builtin_popcountll(present_bits[w]));
		}
		rank_prefix[present_bits.size()] = running;
		slot_orderkeys.assign(running, 0);
		slot_orderdates.assign(running, duckdb::date_t());
		slot_shippriorities.assign(running, 0);
		slot_revenues.assign(running, q3_acc_t {});
		slot_has_revenue.assign(running, 0);
		for (const auto &order : pending) { // later upserts of the same key win
			const auto slot = Rank(order.orderkey);
			slot_orderkeys[slot] = order.orderkey;
			slot_orderdates[slot] = order.orderdate;
			slot_shippriorities[slot] = order.shippriority;
		}
		pending.clear();
		pending.shrink_to_fit();
	}

	bool IsSealed() const {
		return sealed;
	}

	//! Positions i in [0, n) (ascending) whose key keys[idx ? idx[i] : i] is a qualifying dense order.
	size_t SelectQualifying(const int64_t *keys, const uint32_t *idx, size_t n, uint32_t *out) const {
		return q3simd::SelectBits(keys, idx, n, present_bits.data(), static_cast<uint64_t>(capacity), out);
	}

	bool HasOverflow() const {
		return !hash_map.empty();
	}

	size_t Rank(int64_t orderkey) const {
		const auto idx = static_cast<size_t>(orderkey);
		const auto word = idx >> 6;
		const uint64_t below = present_bits[word] & ((uint64_t(1) << (idx & 63)) - 1);
		return rank_prefix[word] + static_cast<size_t>(__builtin_popcountll(below));
	}

	//! Add a lineitem to a bitmap-qualified dense order (caller verified membership via SelectQualifying).
	void AddRevenueRanked(int64_t orderkey, int64_t extendedprice_raw, int64_t discount_raw) {
		const auto slot = Rank(orderkey);
		slot_has_revenue[slot] = 1;
		AddRevenue(slot_revenues[slot], extendedprice_raw, discount_raw);
	}

	bool ProbeAndAddRevenue(int64_t orderkey, int64_t extendedprice_raw, int64_t discount_raw) {
		if (use_dense && orderkey >= 0 && orderkey < capacity) {
			Seal();
			const auto idx = static_cast<size_t>(orderkey);
			if ((present_bits[idx >> 6] & (uint64_t(1) << (idx & 63))) == 0) {
				return false;
			}
			AddRevenueRanked(orderkey, extendedprice_raw, discount_raw);
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
		for (size_t s = 0; s < slot_has_revenue.size(); s++) {
			if (!slot_has_revenue[s]) {
				continue;
			}
			Q3OutputRow row;
			row.orderkey = slot_orderkeys[s];
			row.revenue = FromAccumulator(slot_revenues[s]);
			row.orderdate = slot_orderdates[s];
			row.shippriority = slot_shippriorities[s];
			emit(row);
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
		duckdb::idx_t count = 0;
		for (auto flag : slot_has_revenue) {
			count += flag ? 1 : 0;
		}
		for (const auto &entry : hash_map) {
			count += entry.second.has_revenue ? 1 : 0;
		}
		return count;
	}

	duckdb::idx_t SlotCount() const {
		return slot_orderkeys.size();
	}

	duckdb::idx_t MemoryBytes() const {
		return present_bits.size() * sizeof(uint64_t) + rank_prefix.size() * sizeof(uint32_t) +
		       slot_revenues.size() * sizeof(q3_acc_t) + slot_orderdates.size() * sizeof(duckdb::date_t) +
		       slot_shippriorities.size() * sizeof(int32_t) + slot_orderkeys.size() * sizeof(int64_t) +
		       hash_map.size() * (sizeof(int64_t) + sizeof(OrderState) + sizeof(void *) * 2);
	}

	duckdb::idx_t OverflowCount() const {
		return hash_map.size();
	}

	bool UsingDense() const {
		return use_dense;
	}

private:
	struct PendingOrder {
		int64_t orderkey;
		duckdb::date_t orderdate;
		int32_t shippriority;
	};

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
		present_bits.resize(words, 0);
		capacity = static_cast<int64_t>(words) * 64;
	}

	bool use_dense;
	bool sealed = false;
	int64_t capacity = 0;
	Q3ProfileCounters *profile = nullptr;

	std::vector<uint64_t> present_bits;
	std::vector<uint32_t> rank_prefix;
	std::vector<PendingOrder> pending;
	std::vector<q3_acc_t> slot_revenues;
	std::vector<duckdb::date_t> slot_orderdates;
	std::vector<int32_t> slot_shippriorities;
	std::vector<int64_t> slot_orderkeys;
	std::vector<uint8_t> slot_has_revenue;

	// Overflow for negative / far out-of-range keys.
	std::unordered_map<int64_t, OrderState> hash_map;
};

enum class Q3PathKind : uint8_t { Flat = 0, Dict = 1, Generic = 2 };

class Q3Kernel final : public KernelInstance {
public:
	explicit Q3Kernel(Q3Parameters params_p, Q3ProfileCounters *profile_p = nullptr)
	    : params(std::move(params_p)), profile(profile_p), config(LoadQ3Config()),
	      customers(config.dense_customers),
	      orders(config.dense_orders) {
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
		if (orders.ProbeAndAddRevenue(orderkey, extendedprice_raw, discount_raw)) {
			if (profile) {
				profile->qualifying_lineitems += 1;
				profile->aggregate_writes += 1;
			}
		}
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

		if (shared_sel && customers.UsingDense() && !customers.HasOverflow() && orders.UsingDense()) {
			// SIMD bitmap probe of the customer segment, then upsert only the qualifying orders.
			const uint32_t *sel = shared_sel->data();
			uint32_t hits[STANDARD_VECTOR_SIZE + simd::kSelSlack];
			const auto hit_count = customers.SelectMembers(custkeys, sel, input.size(), hits);
			const int32_t ship_const_val = ship_const ? shippriorities[0] : 0;
			for (size_t h = 0; h < hit_count; h++) {
				const auto row = hits[h];
				const auto idx = sel ? sel[row] : row;
				if (!skip_date && !(orderdates[idx] < params.cutoff_date)) {
					continue;
				}
				const auto shippriority =
				    ship_const ? ship_const_val : shippriorities[GetFastPathIndex(shippriority_vector, row)];
				orders.UpsertOrder(orderkeys[idx], orderdates[idx], shippriority);
				local_qual += 1;
			}
		} else if (shared_sel) {
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

		const bool all_flat = shipdate_vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
		                      orderkey_vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
		                      extendedprice_vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
		                      discount_vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR;
		if ((shared_sel || all_flat) && orders.UsingDense() && !orders.HasOverflow()) {
			// SIMD probe of the qualifying-order bitmap (cache resident); only hits touch the revenue arrays.
			orders.Seal();
			const uint32_t *sel = shared_sel ? shared_sel->data() : nullptr;
			uint32_t hits[STANDARD_VECTOR_SIZE + simd::kSelSlack];
			const auto hit_count = orders.SelectQualifying(orderkeys, sel, input.size(), hits);
			for (size_t h = 0; h < hit_count; h++) {
				const auto row = hits[h];
				const auto idx = sel ? sel[row] : row;
				if (!skip_shipdate && !(shipdates[idx] > params.cutoff_date)) {
					continue;
				}
				orders.AddRevenueRanked(orderkeys[idx], extendedprices[idx], discounts[idx]);
			}
			if (profile) {
				profile->qualifying_lineitems += hit_count;
				profile->aggregate_writes += hit_count;
			}
			return;
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
