#include "backends/duckdb/api/duckdb_scan_adapter.hpp"
#include "backends/duckdb/api/parallel/parallel_kernel.hpp"
#include "backends/duckdb/api/parallel/parallel_runtime.hpp"
#include "tpch/q3/duckdb/base/bitmap_select.hpp"
#include "tpch/q3/duckdb/base/query.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
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
#include "duckdb/parallel/task_executor.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;
constexpr duckdb::idx_t kTopK = 10;
constexpr duckdb::idx_t kOutputBatchSize = 10;
constexpr int64_t kDefaultCustCapacity = 150001;
constexpr int64_t kDefaultOrderCapacity = 6000001;

inline bool IsDenseKey(int64_t key) {
	return key >= 0 && key <= 1'000'000'000;
}

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

inline void AtomicAddAccumulated(q3_acc_t *target, q3_acc_t delta) {
	q3_acc_t old = __atomic_load_n(target, __ATOMIC_RELAXED);
	while (!__atomic_compare_exchange_n(target, &old, static_cast<q3_acc_t>(old + delta), true,
	                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
	}
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

inline bool TestAndSetBit(std::vector<uint64_t> &bits, size_t index) {
	const auto word = index >> 6;
	const auto bit = uint64_t(1) << (index & 63);
	if ((bits[word] & bit) != 0) {
		return false;
	}
	bits[word] |= bit;
	return true;
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

bool StringEquals(const duckdb::string_t &value, const std::string &expected) {
	return value.GetSize() == expected.size() &&
	       std::memcmp(value.GetData(), expected.data(), expected.size()) == 0;
}

struct OrderIdentity {
	duckdb::date_t orderdate;
	int32_t shippriority = 0;
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

struct TopKLess {
	bool operator()(const Q3OutputRow &lhs, const Q3OutputRow &rhs) const {
		return BetterRow(lhs, rhs);
	}
};

class CustomerMembership {
public:
	CustomerMembership() {
		EnsureCapacity(kDefaultCustCapacity);
	}

	void ReserveAtLeast(int64_t min_capacity) {
		EnsureCapacity(min_capacity);
	}

	void Insert(int64_t custkey) {
		if (!IsDenseKey(custkey)) {
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
		if (custkey < 0) {
			return hash_set.find(custkey) != hash_set.end();
		}
		if (custkey < capacity) {
			const auto word = static_cast<uint64_t>(custkey) >> 6;
			const auto bit = uint64_t(1) << (static_cast<uint64_t>(custkey) & 63);
			if ((bits[word] & bit) != 0) {
				return true;
			}
		}
		return hash_set.find(custkey) != hash_set.end();
	}

	void OrBitsFrom(const CustomerMembership &other) {
		if (other.capacity > capacity) {
			EnsureCapacity(other.capacity);
		}
		for (size_t i = 0; i < other.bits.size(); i++) {
			bits[i] |= other.bits[i];
		}
		hash_set.insert(other.hash_set.begin(), other.hash_set.end());
	}

	const std::vector<uint64_t> &Bits() const {
		return bits;
	}

	bool HasOverflow() const {
		return !hash_set.empty();
	}

	//! Positions i in [0, n) whose key keys[idx ? idx[i] : i] is in the dense bitmap (see HasOverflow).
	size_t SelectMembers(const int64_t *keys, const uint32_t *idx, size_t n, uint32_t *out) const {
		return q3simd::SelectBits(keys, idx, n, bits.data(), static_cast<uint64_t>(capacity), out);
	}

	size_t WordCount() const {
		return bits.size();
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
		const auto words = static_cast<size_t>((target + 63) / 64);
		bits.resize(words, 0);
		capacity = static_cast<int64_t>(words) * 64;
	}

	int64_t capacity = 0;
	std::vector<uint64_t> bits;
	std::unordered_set<int64_t> hash_set;
};

struct QualifyingOrder {
	int64_t orderkey;
	duckdb::date_t orderdate;
	int32_t shippriority;
};

class LambdaExecutorTask : public duckdb::BaseExecutorTask {
public:
	LambdaExecutorTask(duckdb::TaskExecutor &executor, std::function<void()> fn_p)
	    : BaseExecutorTask(executor), fn(std::move(fn_p)) {
	}

	void ExecuteTask() override {
		fn();
	}

private:
	std::function<void()> fn;
};

// Qualifying orders: dense presence bitmap over the orderkey domain + rank prefixes (slot = rank of the
// key in the bitmap), compact identity arrays per slot, and a hash map for out-of-range keys.
// Built once between the orders and lineitem phases; read-only afterwards.
class OrderStore {
public:
	OrderStore() = default;

	//! Parallel build over the per-worker order lists (one task per list).
	void Build(duckdb::ClientContext &context, int64_t dense_capacity_hint,
	           std::vector<CacheLinePadded<std::vector<QualifyingOrder>>> &workers) {
		const auto n = workers.size();
		auto run_tasks = [&](const std::function<void(size_t)> &fn) {
			duckdb::TaskExecutor executor(context);
			for (size_t w = 0; w < n; w++) {
				executor.ScheduleTask(duckdb::make_uniq<LambdaExecutorTask>(executor, [&, w]() { fn(w); }));
			}
			executor.WorkOnTasks();
			if (context.interrupted) {
				throw duckdb::InterruptException();
			}
		};
		// Dense domain = estimate from the table size (TPC-H orderkeys span ~4x the order count); any key
		// beyond it takes the hash-map path, which stays exact.
		capacity = ((std::max<int64_t>(dense_capacity_hint, kDefaultOrderCapacity) + 63) / 64) * 64;
		present_bits.assign(static_cast<size_t>(capacity / 64), 0);
		std::vector<std::unordered_map<int64_t, OrderIdentity>> local_hash(n);
		run_tasks([&](size_t w) {
			for (const auto &q : workers[w].value) {
				if (IsDenseKey(q.orderkey) && q.orderkey < capacity) {
					const auto idx = static_cast<size_t>(q.orderkey);
					__atomic_fetch_or(&present_bits[idx >> 6], uint64_t(1) << (idx & 63), __ATOMIC_RELAXED);
				} else {
					local_hash[w][q.orderkey] = OrderIdentity {q.orderdate, q.shippriority};
				}
			}
		});
		for (auto &lh : local_hash) {
			for (auto &entry : lh) {
				hash_map[entry.first] = entry.second;
			}
		}
		rank_prefix.resize(present_bits.size() + 1);
		uint32_t running = 0;
		for (size_t word = 0; word < present_bits.size(); word++) {
			rank_prefix[word] = running;
			running += static_cast<uint32_t>(__builtin_popcountll(present_bits[word]));
		}
		rank_prefix[present_bits.size()] = running;
		slot_count = running;
		slots.reset(new SlotIdentity[running]); // every slot is written below (one per set bit)
		// Order keys are unique in TPC-H (primary key); a duplicate key would map to the same slot.
		run_tasks([&](size_t w) {
			for (const auto &q : workers[w].value) {
				if (IsDenseKey(q.orderkey) && q.orderkey < capacity) {
					slots[Rank(q.orderkey)] = SlotIdentity {q.orderkey, q.orderdate, q.shippriority};
				}
			}
		});
	}

	duckdb::idx_t SlotCount() const {
		return slot_count;
	}

	size_t Rank(int64_t orderkey) const {
		const auto idx = static_cast<size_t>(orderkey);
		const auto word = idx >> 6;
		const uint64_t below = present_bits[word] & ((uint64_t(1) << (idx & 63)) - 1);
		return rank_prefix[word] + static_cast<size_t>(__builtin_popcountll(below));
	}

	//! Slot of a qualifying dense order, -1 if the key is a hash-map (overflow) order, -2 if not qualifying.
	int32_t Probe(int64_t orderkey) const {
		if (orderkey >= 0 && orderkey < capacity) {
			const auto idx = static_cast<size_t>(orderkey);
			if ((present_bits[idx >> 6] & (uint64_t(1) << (idx & 63))) == 0) {
				return -2;
			}
			return static_cast<int32_t>(Rank(orderkey));
		}
		return hash_map.find(orderkey) != hash_map.end() ? -1 : -2;
	}

	bool HasOverflow() const {
		return !hash_map.empty();
	}

	size_t SelectQualifying(const int64_t *keys, const uint32_t *idx, size_t n, uint32_t *out) const {
		return q3simd::SelectBits(keys, idx, n, present_bits.data(), static_cast<uint64_t>(capacity), out);
	}

	const OrderIdentity *FindHash(int64_t orderkey) const {
		auto it = hash_map.find(orderkey);
		return it == hash_map.end() ? nullptr : &it->second;
	}

	int64_t SlotOrderkey(size_t slot) const {
		return slots[slot].orderkey;
	}

	duckdb::date_t SlotOrderdate(size_t slot) const {
		return slots[slot].orderdate;
	}

	int32_t SlotShippriority(size_t slot) const {
		return slots[slot].shippriority;
	}

private:
	int64_t capacity = 0;
	std::vector<uint64_t> present_bits;
	std::vector<uint32_t> rank_prefix;
	struct SlotIdentity {
		int64_t orderkey;
		duckdb::date_t orderdate;
		int32_t shippriority;
	};
	size_t slot_count = 0;
	std::unique_ptr<SlotIdentity[]> slots;
	std::unordered_map<int64_t, OrderIdentity> hash_map;
};

// Per-worker lineitem accumulators: dense per-slot revenue (allocated by the worker on first use, so
// the zeroing is parallel) merged without atomics after the scan.
struct WorkerRevenue {
	std::vector<q3_acc_t> revenue;
	std::vector<uint8_t> has;
	std::vector<uint32_t> touched; // slots with has == 1, in first-touch order
	std::unordered_map<int64_t, q3_acc_t> overflow;
};

struct Q3ParallelProfile {
	int64_t customer_ns = 0;
	int64_t orders_collect_ns = 0;
	int64_t orders_fill_ns = 0;
	int64_t lineitem_ns = 0;
	int64_t merge_ns = 0;
	size_t worker_count = 0;
	size_t slot_count = 0;
	size_t lineitem_rows = 0;
	size_t revenue_contributions = 0;
	size_t active_slots = 0;
	size_t lineitem_flat_rows = 0;
	size_t lineitem_fallback_rows = 0;
};

void PrintQ3ParallelProfile(const Q3ParallelProfile &profile) {
	std::cerr << "q3_parallel_profile customer_ns=" << profile.customer_ns
	          << " orders_collect_ns=" << profile.orders_collect_ns << " orders_fill_ns=" << profile.orders_fill_ns
	          << " lineitem_ns=" << profile.lineitem_ns << " merge_ns=" << profile.merge_ns
	          << " worker_count=" << profile.worker_count << " slot_count=" << profile.slot_count
	          << " lineitem_rows=" << profile.lineitem_rows << " revenue_contributions=" << profile.revenue_contributions
	          << " active_slots=" << profile.active_slots << " lineitem_flat_rows=" << profile.lineitem_flat_rows
	          << " lineitem_fallback_rows=" << profile.lineitem_fallback_rows << "\n";
}

//! Qualifying orders of one orders chunk: orderdate < cutoff AND custkey in the segment set.
void CollectOrdersChunkImpl(const CustomerMembership &customers, const Q3Parameters &params, duckdb::DataChunk &input,
                            std::vector<QualifyingOrder> &local) {
	auto &orderdate_vector = input.data[Q3OrdersChunkLayout::ORDERDATE_INDEX];
	auto &orderkey_vector = input.data[Q3OrdersChunkLayout::ORDERKEY_INDEX];
	auto &custkey_vector = input.data[Q3OrdersChunkLayout::CUSTKEY_INDEX];
	auto &shippriority_vector = input.data[Q3OrdersChunkLayout::SHIPPRIORITY_INDEX];

	// SIMD fast path: all-valid dictionary columns sharing one selection (shippriority may be constant),
	// or all-valid flat columns.
	const auto type = orderdate_vector.GetVectorType();
	const uint32_t *sel = nullptr;
	bool batch = false;
	const int64_t *orderkeys = nullptr, *custkeys = nullptr;
	const duckdb::date_t *orderdates = nullptr;
	if (!customers.HasOverflow() && IsFlatAllValid(orderdate_vector) && IsFlatAllValid(orderkey_vector) &&
	    IsFlatAllValid(custkey_vector) && IsFlatAllValid(shippriority_vector)) {
		batch = true;
		orderdates = duckdb::FlatVector::GetData<duckdb::date_t>(orderdate_vector);
		orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
	} else if (!customers.HasOverflow() && type == duckdb::VectorType::DICTIONARY_VECTOR &&
	           IsDictionaryOverFlat(orderdate_vector) && IsDictionaryOverFlat(orderkey_vector) &&
	           IsDictionaryOverFlat(custkey_vector) &&
	           duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderdate_vector)).AllValid() &&
	           duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector)).AllValid() &&
	           duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(custkey_vector)).AllValid() &&
	           ((shippriority_vector.GetVectorType() == duckdb::VectorType::CONSTANT_VECTOR &&
	             !duckdb::ConstantVector::IsNull(shippriority_vector)) ||
	            IsFlatAllValid(shippriority_vector))) {
		auto &sel0 = duckdb::DictionaryVector::SelVector(orderdate_vector);
		auto &sel1 = duckdb::DictionaryVector::SelVector(orderkey_vector);
		auto &sel2 = duckdb::DictionaryVector::SelVector(custkey_vector);
		if (sel0.data() && sel0.data() == sel1.data() && sel1.data() == sel2.data()) {
			batch = true;
			sel = sel0.data();
			orderdates = duckdb::FlatVector::GetData<duckdb::date_t>(duckdb::DictionaryVector::Child(orderdate_vector));
			orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
			custkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(custkey_vector));
		}
	}
	if (batch && shippriority_vector.GetVectorType() == duckdb::VectorType::CONSTANT_VECTOR &&
	    duckdb::ConstantVector::IsNull(shippriority_vector)) {
		batch = false;
	}
	if (batch && shippriority_vector.GetVectorType() != duckdb::VectorType::CONSTANT_VECTOR &&
	    !IsFlatAllValid(shippriority_vector)) {
		batch = false;
	}
	if (batch && input.size() <= STANDARD_VECTOR_SIZE) {
		const bool ship_const = shippriority_vector.GetVectorType() == duckdb::VectorType::CONSTANT_VECTOR;
		const auto *shippriorities = ship_const ? duckdb::ConstantVector::GetData<int32_t>(shippriority_vector)
		                                        : duckdb::FlatVector::GetData<int32_t>(shippriority_vector);
		uint32_t hits[STANDARD_VECTOR_SIZE + simd::kSelSlack];
		const auto hit_count = customers.SelectMembers(custkeys, sel, input.size(), hits);
		for (size_t h = 0; h < hit_count; h++) {
			const auto row = hits[h];
			const auto idx = sel ? sel[row] : row;
			// The orderdate filter is also pushed into the scan; re-check it exactly (cheap: hits only).
			if (!(orderdates[idx] < params.cutoff_date)) {
				continue;
			}
			// Dictionary shippriority is only handled when constant; flat is indexed by row.
			const auto shippriority = ship_const ? shippriorities[0] : shippriorities[row];
			local.push_back(QualifyingOrder {orderkeys[idx], orderdates[idx], shippriority});
		}
		return;
	}

	duckdb::UnifiedVectorFormat orderdate_format;
	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat custkey_format;
	duckdb::UnifiedVectorFormat shippriority_format;
	orderdate_vector.ToUnifiedFormat(input.size(), orderdate_format);
	orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
	custkey_vector.ToUnifiedFormat(input.size(), custkey_format);
	shippriority_vector.ToUnifiedFormat(input.size(), shippriority_format);
	auto g_orderdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(orderdate_format);
	auto g_orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto g_custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
	auto g_shippriorities = duckdb::UnifiedVectorFormat::GetData<int32_t>(shippriority_format);
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
		const auto orderdate = g_orderdates[orderdate_index];
		if (!(orderdate < params.cutoff_date)) {
			continue;
		}
		if (!customers.Contains(g_custkeys[custkey_index])) {
			continue;
		}
		local.push_back(
		    QualifyingOrder {g_orderkeys[orderkey_index], orderdate, g_shippriorities[shippriority_index]});
	}
}

class Q3ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q3ParallelKernel(Q3Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 3 || !bound_inputs[0] || !bound_inputs[1] || !bound_inputs[2]) {
			throw duckdb::InternalException("Q3 parallel expects customer + orders + lineitem");
		}

		using clock = std::chrono::steady_clock;
		Q3ParallelProfile profile;
		const auto worker_count = ParallelWorkerCount(context);
		profile.worker_count = static_cast<size_t>(worker_count);

		CustomerMembership customers;
		{
			const auto customer_t0 = clock::now();
			std::vector<CacheLinePadded<CustomerMembership>> customer_workers(worker_count);
			RunParallelChunkScan(context, *bound_inputs[0], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &input) {
				                     auto &local = customer_workers[worker_id].value;
				                     duckdb::UnifiedVectorFormat mkt_format;
				                     duckdb::UnifiedVectorFormat custkey_format;
				                     input.data[Q3CustomerChunkLayout::MKTSEGMENT_INDEX].ToUnifiedFormat(input.size(),
				                                                                                         mkt_format);
				                     input.data[Q3CustomerChunkLayout::CUSTKEY_INDEX].ToUnifiedFormat(input.size(),
				                                                                                      custkey_format);
				                     auto segments = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(mkt_format);
				                     auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
				                     for (duckdb::idx_t row = 0; row < input.size(); row++) {
					                     const auto mkt_index = mkt_format.sel->get_index(row);
					                     const auto custkey_index = custkey_format.sel->get_index(row);
					                     if ((!mkt_format.validity.AllValid() &&
					                          !mkt_format.validity.RowIsValidUnsafe(mkt_index)) ||
					                         (!custkey_format.validity.AllValid() &&
					                          !custkey_format.validity.RowIsValidUnsafe(custkey_index))) {
						                     continue;
					                     }
					                     if (!StringEquals(segments[mkt_index], params.market_segment)) {
						                     continue;
					                     }
					                     local.Insert(custkeys[custkey_index]);
				                     }
			                     });
			for (const auto &w : customer_workers) {
				customers.OrBitsFrom(w.value);
			}
			profile.customer_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - customer_t0).count();
		}
		// Freeze: customers is immutable for the orders build and lineitem probe.

		std::vector<CacheLinePadded<std::vector<QualifyingOrder>>> order_workers(worker_count);
		{
			const auto orders_collect_t0 = clock::now();
			RunParallelChunkScan(context, *bound_inputs[1], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &input) {
				                     auto &local = order_workers[worker_id].value;
				                     CollectOrdersChunkImpl(customers, params, input, local);
			                     });
			profile.orders_collect_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() -
			                                                                                orders_collect_t0)
			                                .count();
		}

		{
			const auto orders_fill_t0 = clock::now();
			orders.Build(context, EstimateTpchDenseOrderCapacity(context, *bound_inputs[1]), order_workers);
			profile.orders_fill_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - orders_fill_t0).count();
		}
		// Drop collected order rows before allocating lineitem worker state (peak RSS).
		order_workers.clear();
		order_workers.shrink_to_fit();

		const auto slot_count = orders.SlotCount();
		profile.slot_count = static_cast<size_t>(slot_count);

		std::vector<CacheLinePadded<WorkerRevenue>> workers(worker_count);

		{
			const auto lineitem_t0 = clock::now();
			std::vector<size_t> lineitem_rows(worker_count, 0);
			std::vector<size_t> lineitem_contribs(worker_count, 0);
			RunParallelChunkScan(context, *bound_inputs[2], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &input) {
				                     auto &local = workers[worker_id].value;
				                     if (local.revenue.size() != slot_count) {
					                     local.revenue.assign(slot_count, q3_acc_t {});
					                     local.has.assign(slot_count, 0);
				                     }
				                     lineitem_rows[worker_id] += input.size();
				                     lineitem_contribs[worker_id] += ConsumeLineitemChunk(local, input);
			                     });
			for (size_t w = 0; w < worker_count; w++) {
				profile.lineitem_rows += lineitem_rows[w];
				profile.revenue_contributions += lineitem_contribs[w];
			}
			profile.lineitem_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - lineitem_t0).count();
		}

		{
			// Merge the per-worker arrays (plain adds; no atomics were needed during the scan). The first
			// worker that scanned anything donates its arrays; the others only add their touched slots.
			const auto merge_t0 = clock::now();
			bool have_base = false;
			for (auto &w : workers) {
				if (w.value.revenue.size() != slot_count) {
					continue;
				}
				if (!have_base) {
					merged_revenues = std::move(w.value.revenue);
					merged_has = std::move(w.value.has);
					active_slots = std::move(w.value.touched);
					have_base = true;
				} else {
					for (const auto slot : w.value.touched) {
						if (!merged_has[slot]) {
							merged_has[slot] = 1;
							active_slots.push_back(slot);
						}
						AddAccumulated(merged_revenues[slot], w.value.revenue[slot]);
					}
				}
			}
			for (const auto &w : workers) {
				for (const auto &entry : w.value.overflow) {
					AddAccumulated(merged_overflow[entry.first], entry.second);
				}
			}
			profile.active_slots = active_slots.size() + merged_overflow.size();
			profile.merge_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - merge_t0).count();
		}

		if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
			PrintQ3ParallelProfile(profile);
		}
	}

	void Finalize() override {
		if (finalized) {
			return;
		}
		std::priority_queue<Q3OutputRow, std::vector<Q3OutputRow>, TopKLess> heap;
		auto consider = [&](const Q3OutputRow &row) {
			if (heap.size() < kTopK) {
				heap.push(row);
				return;
			}
			if (BetterRow(row, heap.top())) {
				heap.pop();
				heap.push(row);
			}
		};

		for (const auto slot : active_slots) {
			const size_t s = slot;
			Q3OutputRow row;
			row.orderkey = orders.SlotOrderkey(s);
			row.revenue = FromAccumulator(merged_revenues[s]);
			row.orderdate = orders.SlotOrderdate(s);
			row.shippriority = orders.SlotShippriority(s);
			consider(row);
		}
		for (const auto &entry : merged_overflow) {
			const auto *identity = orders.FindHash(entry.first);
			if (!identity) {
				continue;
			}
			Q3OutputRow row;
			row.orderkey = entry.first;
			row.revenue = FromAccumulator(entry.second);
			row.orderdate = identity->orderdate;
			row.shippriority = identity->shippriority;
			consider(row);
		}

		ordered_rows.clear();
		ordered_rows.reserve(heap.size());
		while (!heap.empty()) {
			ordered_rows.push_back(heap.top());
			heap.pop();
		}
		std::sort(ordered_rows.begin(), ordered_rows.end(), BetterRow);
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

	//! Accumulate one lineitem chunk into this worker's slot arrays; returns the number of qualifying rows.
	size_t ConsumeLineitemChunk(WorkerRevenue &local, duckdb::DataChunk &input) const {
		auto &shipdate_vector = input.data[Q3LineitemChunkLayout::SHIPDATE_INDEX];
		auto &orderkey_vector = input.data[Q3LineitemChunkLayout::ORDERKEY_INDEX];
		auto &extendedprice_vector = input.data[Q3LineitemChunkLayout::EXTENDEDPRICE_INDEX];
		auto &discount_vector = input.data[Q3LineitemChunkLayout::DISCOUNT_INDEX];
		// DICTIONARY shipdate comes from base storage that already applied the pushed-down filter.
		const bool skip_residual = shipdate_vector.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR;
		size_t contributions = 0;

		const duckdb::date_t *shipdates = nullptr;
		const int64_t *orderkeys = nullptr, *extendedprices = nullptr, *discounts = nullptr;
		const uint32_t *sel = nullptr;
		bool batch = false;
		if (!orders.HasOverflow() && input.size() <= STANDARD_VECTOR_SIZE) {
			if (IsFlatAllValid(shipdate_vector) && IsFlatAllValid(orderkey_vector) &&
			    IsFlatAllValid(extendedprice_vector) && IsFlatAllValid(discount_vector)) {
				batch = true;
				shipdates = duckdb::FlatVector::GetData<duckdb::date_t>(shipdate_vector);
				orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
				extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vector);
				discounts = duckdb::FlatVector::GetData<int64_t>(discount_vector);
			} else if (IsDictionaryOverFlat(shipdate_vector) && IsDictionaryOverFlat(orderkey_vector) &&
			           IsDictionaryOverFlat(extendedprice_vector) && IsDictionaryOverFlat(discount_vector) &&
			           duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(shipdate_vector)).AllValid() &&
			           duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector)).AllValid() &&
			           duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(extendedprice_vector)).AllValid() &&
			           duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(discount_vector)).AllValid()) {
				auto &sel0 = duckdb::DictionaryVector::SelVector(shipdate_vector);
				auto &sel1 = duckdb::DictionaryVector::SelVector(orderkey_vector);
				auto &sel2 = duckdb::DictionaryVector::SelVector(extendedprice_vector);
				auto &sel3 = duckdb::DictionaryVector::SelVector(discount_vector);
				if (sel0.data() && sel0.data() == sel1.data() && sel1.data() == sel2.data() &&
				    sel2.data() == sel3.data()) {
					batch = true;
					sel = sel0.data();
					shipdates =
					    duckdb::FlatVector::GetData<duckdb::date_t>(duckdb::DictionaryVector::Child(shipdate_vector));
					orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
					extendedprices =
					    duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(extendedprice_vector));
					discounts = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(discount_vector));
				}
			}
		}
		if (batch) {
			// SIMD probe of the (L2-resident) qualifying-order bitmap; only hits touch the revenue arrays.
			uint32_t hits[STANDARD_VECTOR_SIZE + simd::kSelSlack];
			const auto hit_count = orders.SelectQualifying(orderkeys, sel, input.size(), hits);
			for (size_t h = 0; h < hit_count; h++) {
				const auto row = hits[h];
				const auto idx = sel ? sel[row] : row;
				if (!skip_residual && !(shipdates[idx] > params.cutoff_date)) {
					continue;
				}
				const auto slot = orders.Rank(orderkeys[idx]);
				AddRevenue(local.revenue[slot], extendedprices[idx], discounts[idx]);
				if (!local.has[slot]) {
					local.has[slot] = 1;
					local.touched.push_back(static_cast<uint32_t>(slot));
				}
				contributions++;
			}
			return contributions;
		}

		duckdb::UnifiedVectorFormat shipdate_format;
		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat extendedprice_format;
		duckdb::UnifiedVectorFormat discount_format;
		shipdate_vector.ToUnifiedFormat(input.size(), shipdate_format);
		orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
		extendedprice_vector.ToUnifiedFormat(input.size(), extendedprice_format);
		discount_vector.ToUnifiedFormat(input.size(), discount_format);
		auto g_shipdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdate_format);
		auto g_orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto g_extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
		auto g_discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			const auto shipdate_index = shipdate_format.sel->get_index(row);
			const auto extendedprice_index = extendedprice_format.sel->get_index(row);
			const auto discount_index = discount_format.sel->get_index(row);
			if ((!shipdate_format.validity.AllValid() && !shipdate_format.validity.RowIsValidUnsafe(shipdate_index)) ||
			    (!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
			    (!extendedprice_format.validity.AllValid() &&
			     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
			    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index))) {
				continue;
			}
			const int32_t slot = orders.Probe(g_orderkeys[orderkey_index]);
			if (slot == -2) {
				continue;
			}
			if (!skip_residual && !(g_shipdates[shipdate_index] > params.cutoff_date)) {
				continue;
			}
			if (slot >= 0) {
				AddRevenue(local.revenue[static_cast<size_t>(slot)], g_extendedprices[extendedprice_index],
				           g_discounts[discount_index]);
				if (!local.has[static_cast<size_t>(slot)]) {
					local.has[static_cast<size_t>(slot)] = 1;
					local.touched.push_back(static_cast<uint32_t>(slot));
				}
			} else {
				AddRevenue(local.overflow[g_orderkeys[orderkey_index]], g_extendedprices[extendedprice_index],
				           g_discounts[discount_index]);
			}
			contributions++;
		}
		return contributions;
	}

private:
	Q3Parameters params;
	OrderStore orders;
	std::vector<q3_acc_t> merged_revenues;
	std::vector<uint8_t> merged_has;
	std::vector<uint32_t> active_slots;
	std::unordered_map<int64_t, q3_acc_t> merged_overflow;
	std::vector<Q3OutputRow> ordered_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ3ParallelKernel(const Q3Parameters &params) {
	return duckdb::make_uniq<Q3ParallelKernel>(params);
}

} // namespace accelerator_codegen
