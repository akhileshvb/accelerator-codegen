#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/parallel/parallel_kernel.hpp"
#include "accelerator_codegen/tpch_q3.hpp"

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

// Slot-map OrderStore (identity only). Revenues are accumulated in worker-local arrays.
class OrderStore {
public:
	OrderStore() {
		EnsureCapacity(kDefaultOrderCapacity);
		slot_orderkeys.reserve(200000);
		slot_orderdates.reserve(200000);
		slot_shippriorities.reserve(200000);
	}

	~OrderStore() {
		delete[] slot_of_key;
	}

	OrderStore(const OrderStore &) = delete;
	OrderStore &operator=(const OrderStore &) = delete;

	void ReserveKeyCapacity(int64_t min_capacity) {
		EnsureCapacity(min_capacity);
	}

	void AllocateSlots(size_t slot_count) {
		slot_orderkeys.resize(slot_count);
		slot_orderdates.resize(slot_count);
		slot_shippriorities.resize(slot_count);
	}

	void WriteSlot(size_t slot, int64_t orderkey, duckdb::date_t orderdate, int32_t shippriority) {
		if (!IsDenseKey(orderkey)) {
			hash_map[orderkey] = OrderIdentity {orderdate, shippriority};
			return;
		}
		if (orderkey >= capacity) {
			EnsureCapacity(orderkey + 1);
		}
		slot_orderkeys[slot] = orderkey;
		slot_orderdates[slot] = orderdate;
		slot_shippriorities[slot] = shippriority;
		slot_of_key[static_cast<size_t>(orderkey)] = static_cast<int32_t>(slot);
	}

	void MergeHashIdentity(int64_t orderkey, OrderIdentity id) {
		hash_map[orderkey] = id;
	}

	void UpsertOrder(int64_t orderkey, duckdb::date_t orderdate, int32_t shippriority) {
		if (!IsDenseKey(orderkey)) {
			hash_map[orderkey] = OrderIdentity {orderdate, shippriority};
			return;
		}
		if (orderkey >= capacity) {
			EnsureCapacity(orderkey + 1);
		}
		const auto key_idx = static_cast<size_t>(orderkey);
		const auto existing = slot_of_key[key_idx];
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
	}

	duckdb::idx_t SlotCount() const {
		return slot_orderkeys.size();
	}

	bool ProbeQualifies(int64_t orderkey, int32_t *out_slot) const {
		*out_slot = -1;
		if (orderkey >= 0 && orderkey < capacity) {
			const auto stored = slot_of_key[static_cast<size_t>(orderkey)];
			if (stored < 0) {
				return false;
			}
			*out_slot = stored;
			return true;
		}
		return hash_map.find(orderkey) != hash_map.end();
	}

	const OrderIdentity *FindHash(int64_t orderkey) const {
		auto it = hash_map.find(orderkey);
		return it == hash_map.end() ? nullptr : &it->second;
	}

	int64_t SlotOrderkey(size_t slot) const {
		return slot_orderkeys[slot];
	}

	duckdb::date_t SlotOrderdate(size_t slot) const {
		return slot_orderdates[slot];
	}

	int32_t SlotShippriority(size_t slot) const {
		return slot_shippriorities[slot];
	}

	const std::unordered_map<int64_t, OrderIdentity> &HashMap() const {
		return hash_map;
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
		const auto rounded = static_cast<int64_t>(words) * 64;
		auto *new_map = new int32_t[static_cast<size_t>(rounded)];
		if (capacity > 0) {
			std::memcpy(new_map, slot_of_key, static_cast<size_t>(capacity) * sizeof(int32_t));
		}
		std::memset(new_map + capacity, 0xFF, static_cast<size_t>(rounded - capacity) * sizeof(int32_t));
		delete[] slot_of_key;
		slot_of_key = new_map;
		capacity = rounded;
	}

	int64_t capacity = 0;
	int32_t *slot_of_key = nullptr;
	std::vector<int64_t> slot_orderkeys;
	std::vector<duckdb::date_t> slot_orderdates;
	std::vector<int32_t> slot_shippriorities;
	std::unordered_map<int64_t, OrderIdentity> hash_map;
};

// Per-worker lineitem state: sparse touch tracking only (no dense revenue replicas).
// With __int128, revenues accumulate into a shared atomic array; workers keep has_bits + touched.
// Without __int128, revenues are allocated uninitialized and zeroed only on first touch.
struct WorkerRevenue {
#if !defined(__SIZEOF_INT128__)
	std::unique_ptr<q3_acc_t[]> revenues;
#endif
	std::vector<uint64_t> has_bits;
	std::vector<int32_t> touched;
	std::unordered_map<int64_t, q3_acc_t> overflow;
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
		const bool profile = []() {
			const char *env = std::getenv("ACCEL_PARALLEL_PROFILE");
			return env && std::string(env) == "1";
		}();
		int64_t profile_customer_ns = 0;
		int64_t profile_orders_collect_ns = 0;
		int64_t profile_orders_fill_ns = 0;
		int64_t profile_lineitem_ns = 0;
		int64_t profile_merge_ns = 0;

		const auto worker_count = ParallelWorkerCount(context);
		if (const char *audit = std::getenv("ACCEL_PARALLEL_AUDIT"); audit && audit[0] == '1') {
			std::cerr << "ACCEL_PARALLEL_AUDIT q3 parallel_build workers=" << worker_count
			          << " phases=customer,orders_collect,orders_fill,lineitem\n";
		}

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
			if (profile) {
				profile_customer_ns =
				    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - customer_t0).count();
			}
		}
		// Freeze: customers is immutable for the orders build and lineitem probe.

		std::vector<CacheLinePadded<std::vector<QualifyingOrder>>> order_workers(worker_count);
		{
			const auto orders_collect_t0 = clock::now();
			RunParallelChunkScan(context, *bound_inputs[1], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &input) {
				                     auto &local = order_workers[worker_id].value;
				                     duckdb::UnifiedVectorFormat orderdate_format;
				                     duckdb::UnifiedVectorFormat orderkey_format;
				                     duckdb::UnifiedVectorFormat custkey_format;
				                     duckdb::UnifiedVectorFormat shippriority_format;
				                     input.data[Q3OrdersChunkLayout::ORDERDATE_INDEX].ToUnifiedFormat(input.size(),
				                                                                                      orderdate_format);
				                     input.data[Q3OrdersChunkLayout::ORDERKEY_INDEX].ToUnifiedFormat(input.size(),
				                                                                                     orderkey_format);
				                     input.data[Q3OrdersChunkLayout::CUSTKEY_INDEX].ToUnifiedFormat(input.size(),
				                                                                                    custkey_format);
				                     input.data[Q3OrdersChunkLayout::SHIPPRIORITY_INDEX].ToUnifiedFormat(
				                         input.size(), shippriority_format);
				                     auto orderdates =
				                         duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(orderdate_format);
				                     auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
				                     auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
				                     auto shippriorities =
				                         duckdb::UnifiedVectorFormat::GetData<int32_t>(shippriority_format);
				                     for (duckdb::idx_t row = 0; row < input.size(); row++) {
					                     const auto orderdate_index = orderdate_format.sel->get_index(row);
					                     const auto orderkey_index = orderkey_format.sel->get_index(row);
					                     const auto custkey_index = custkey_format.sel->get_index(row);
					                     const auto shippriority_index = shippriority_format.sel->get_index(row);
					                     if ((!orderdate_format.validity.AllValid() &&
					                          !orderdate_format.validity.RowIsValidUnsafe(orderdate_index)) ||
					                         (!orderkey_format.validity.AllValid() &&
					                          !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
					                         (!custkey_format.validity.AllValid() &&
					                          !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
					                         (!shippriority_format.validity.AllValid() &&
					                          !shippriority_format.validity.RowIsValidUnsafe(shippriority_index))) {
						                     continue;
					                     }
					                     const auto orderdate = orderdates[orderdate_index];
					                     if (!(orderdate < params.cutoff_date)) {
						                     continue;
					                     }
					                     if (!customers.Contains(custkeys[custkey_index])) {
						                     continue;
					                     }
					                     local.push_back(QualifyingOrder {orderkeys[orderkey_index], orderdate,
					                                                      shippriorities[shippriority_index]});
				                     }
			                     });
			if (profile) {
				profile_orders_collect_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() -
				                                                                                orders_collect_t0)
				                                .count();
			}
		}

		std::vector<size_t> sizes(worker_count, 0);
		std::vector<size_t> bases(worker_count, 0);
		size_t total_slots = 0;
		int64_t max_dense_orderkey = 0;
		for (duckdb::idx_t w = 0; w < worker_count; w++) {
			size_t dense = 0;
			for (const auto &q : order_workers[w].value) {
				if (IsDenseKey(q.orderkey)) {
					dense++;
					if (q.orderkey > max_dense_orderkey) {
						max_dense_orderkey = q.orderkey;
					}
				}
			}
			sizes[w] = dense;
			bases[w] = total_slots;
			total_slots += dense;
		}

		const int64_t estimated_capacity = EstimateTpchDenseOrderCapacity(context, *bound_inputs[1]);
		const int64_t observed_capacity = max_dense_orderkey > 0 ? max_dense_orderkey + 1 : kDefaultOrderCapacity;
		orders.ReserveKeyCapacity(std::max(estimated_capacity, observed_capacity));
		orders.AllocateSlots(total_slots);
		{
			const auto orders_fill_t0 = clock::now();
			duckdb::TaskExecutor fill_executor(context);
			for (duckdb::idx_t w = 0; w < worker_count; w++) {
				fill_executor.ScheduleTask(duckdb::make_uniq<LambdaExecutorTask>(fill_executor, [&, w]() {
					size_t slot = bases[w];
					for (const auto &q : order_workers[w].value) {
						if (!IsDenseKey(q.orderkey)) {
							continue;
						}
						orders.WriteSlot(slot++, q.orderkey, q.orderdate, q.shippriority);
					}
				}));
			}
			fill_executor.WorkOnTasks();
			if (context.interrupted) {
				throw duckdb::InterruptException();
			}
			if (profile) {
				profile_orders_fill_ns =
				    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - orders_fill_t0).count();
			}
		}
		for (const auto &w : order_workers) {
			for (const auto &q : w.value) {
				if (!IsDenseKey(q.orderkey)) {
					orders.MergeHashIdentity(q.orderkey, OrderIdentity {q.orderdate, q.shippriority});
				}
			}
		}
		// Drop collected order rows before allocating lineitem worker state (peak RSS).
		order_workers.clear();
		order_workers.shrink_to_fit();

		const auto slot_count = orders.SlotCount();
		const auto has_words = (slot_count + 63) / 64;
		merged_revenues.assign(slot_count, q3_acc_t {});
		merged_has.assign(slot_count, 0);
		all_touched.clear();

		std::vector<CacheLinePadded<WorkerRevenue>> workers(worker_count);
		for (auto &w : workers) {
			w.value.has_bits.assign(has_words, 0);
			w.value.touched.reserve(4096);
#if !defined(__SIZEOF_INT128__)
			w.value.revenues = std::make_unique<q3_acc_t[]>(slot_count);
#endif
		}

		auto add_revenue = [&](WorkerRevenue &local, int64_t orderkey, int64_t extendedprice_raw,
		                       int64_t discount_raw) {
			int32_t slot = -1;
			if (!orders.ProbeQualifies(orderkey, &slot)) {
				return;
			}
			if (slot >= 0) {
				const auto s = static_cast<size_t>(slot);
#if defined(__SIZEOF_INT128__)
				q3_acc_t delta = 0;
				AddRevenue(delta, extendedprice_raw, discount_raw);
				AtomicAddAccumulated(&merged_revenues[s], delta);
				if (TestAndSetBit(local.has_bits, s)) {
					local.touched.push_back(slot);
				}
#else
				if (TestAndSetBit(local.has_bits, s)) {
					local.revenues[s] = q3_acc_t {};
					local.touched.push_back(slot);
				}
				AddRevenue(local.revenues[s], extendedprice_raw, discount_raw);
#endif
				return;
			}
			AddRevenue(local.overflow[orderkey], extendedprice_raw, discount_raw);
		};

		{
			const auto lineitem_t0 = clock::now();
			RunParallelChunkScan(context, *bound_inputs[2], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &input) {
				                     auto &local = workers[worker_id].value;
				                     auto &shipdate_vector = input.data[Q3LineitemChunkLayout::SHIPDATE_INDEX];
				                     const bool skip_residual =
				                         shipdate_vector.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR;

				                     if (IsFlatAllValid(shipdate_vector) &&
				                         IsFlatAllValid(input.data[Q3LineitemChunkLayout::ORDERKEY_INDEX]) &&
				                         IsFlatAllValid(input.data[Q3LineitemChunkLayout::EXTENDEDPRICE_INDEX]) &&
				                         IsFlatAllValid(input.data[Q3LineitemChunkLayout::DISCOUNT_INDEX])) {
					                     auto shipdates = duckdb::FlatVector::GetData<duckdb::date_t>(shipdate_vector);
					                     auto orderkeys = duckdb::FlatVector::GetData<int64_t>(
					                         input.data[Q3LineitemChunkLayout::ORDERKEY_INDEX]);
					                     auto extendedprices = duckdb::FlatVector::GetData<int64_t>(
					                         input.data[Q3LineitemChunkLayout::EXTENDEDPRICE_INDEX]);
					                     auto discounts = duckdb::FlatVector::GetData<int64_t>(
					                         input.data[Q3LineitemChunkLayout::DISCOUNT_INDEX]);
					                     for (duckdb::idx_t row = 0; row < input.size(); row++) {
						                     if (!skip_residual && !(shipdates[row] > params.cutoff_date)) {
							                     continue;
						                     }
						                     add_revenue(local, orderkeys[row], extendedprices[row], discounts[row]);
					                     }
					                     return;
				                     }

				                     duckdb::UnifiedVectorFormat shipdate_format;
				                     duckdb::UnifiedVectorFormat orderkey_format;
				                     duckdb::UnifiedVectorFormat extendedprice_format;
				                     duckdb::UnifiedVectorFormat discount_format;
				                     shipdate_vector.ToUnifiedFormat(input.size(), shipdate_format);
				                     input.data[Q3LineitemChunkLayout::ORDERKEY_INDEX].ToUnifiedFormat(input.size(),
				                                                                                        orderkey_format);
				                     input.data[Q3LineitemChunkLayout::EXTENDEDPRICE_INDEX].ToUnifiedFormat(
				                         input.size(), extendedprice_format);
				                     input.data[Q3LineitemChunkLayout::DISCOUNT_INDEX].ToUnifiedFormat(
				                         input.size(), discount_format);
				                     auto shipdates =
				                         duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdate_format);
				                     auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
				                     auto extendedprices =
				                         duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
				                     auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
				                     for (duckdb::idx_t row = 0; row < input.size(); row++) {
					                     const auto shipdate_index = shipdate_format.sel->get_index(row);
					                     const auto orderkey_index = orderkey_format.sel->get_index(row);
					                     const auto extendedprice_index = extendedprice_format.sel->get_index(row);
					                     const auto discount_index = discount_format.sel->get_index(row);
					                     if ((!shipdate_format.validity.AllValid() &&
					                          !shipdate_format.validity.RowIsValidUnsafe(shipdate_index)) ||
					                         (!orderkey_format.validity.AllValid() &&
					                          !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
					                         (!extendedprice_format.validity.AllValid() &&
					                          !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
					                         (!discount_format.validity.AllValid() &&
					                          !discount_format.validity.RowIsValidUnsafe(discount_index))) {
						                     continue;
					                     }
					                     if (!skip_residual && !(shipdates[shipdate_index] > params.cutoff_date)) {
						                     continue;
					                     }
					                     add_revenue(local, orderkeys[orderkey_index],
					                                 extendedprices[extendedprice_index], discounts[discount_index]);
				                     }
			                     });
			if (profile) {
				profile_lineitem_ns =
				    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - lineitem_t0).count();
			}
		}

		{
			const auto merge_t0 = clock::now();
			all_touched.reserve(slot_count / 8 + 1);
			for (const auto &w : workers) {
				for (const int32_t slot : w.value.touched) {
					const auto s = static_cast<size_t>(slot);
					if (!merged_has[s]) {
						merged_has[s] = 1;
						all_touched.push_back(slot);
					}
#if !defined(__SIZEOF_INT128__)
					AddAccumulated(merged_revenues[s], w.value.revenues[s]);
#endif
				}
				for (const auto &entry : w.value.overflow) {
					AddAccumulated(merged_overflow[entry.first], entry.second);
				}
			}
			if (profile) {
				profile_merge_ns =
				    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - merge_t0).count();
			}
		}

		if (profile) {
			std::cerr << "q3_parallel_profile customer_ns=" << profile_customer_ns
			          << " orders_collect_ns=" << profile_orders_collect_ns
			          << " orders_fill_ns=" << profile_orders_fill_ns << " lineitem_ns=" << profile_lineitem_ns
			          << " merge_ns=" << profile_merge_ns << " slot_count=" << slot_count
			          << " worker_count=" << worker_count << "\n";
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

		for (const int32_t slot : all_touched) {
			const auto s = static_cast<size_t>(slot);
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

private:
	Q3Parameters params;
	OrderStore orders;
	std::vector<q3_acc_t> merged_revenues;
	std::vector<uint8_t> merged_has;
	std::vector<int32_t> all_touched;
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
