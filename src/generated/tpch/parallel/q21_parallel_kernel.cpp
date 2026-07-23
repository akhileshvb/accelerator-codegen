#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/parallel/parallel_kernel.hpp"
#include "accelerator_codegen/parallel/parallel_runtime.hpp"
#include "accelerator_codegen/tpch_q21.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/parallel/task.hpp"
#include "duckdb/parallel/task_executor.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr size_t kMinPartitions = 32;
constexpr size_t kMaxPartitions = 256;
constexpr size_t kPartitionsPerWorker = 6;
constexpr size_t kDensePartitionSpanLimit = 2'000'000;

enum OrderSummaryFlag : uint8_t {
	kInitialized = 1,
	kOtherSupplier = 2,
	kLateSeen = 4,
	kOtherLateSupplier = 8,
};

struct OrderSummary {
	uint32_t first_supplier = 0;
	uint32_t first_late_supplier = 0;
	uint32_t first_late_count = 0;
	uint32_t line_count = 0;
	uint8_t flags = 0;
};

struct Q21OutputRow {
	std::string name;
	int64_t numwait = 0;
};

struct CompactFragment {
	std::vector<uint32_t> orderkeys;
	std::vector<uint32_t> suppkeys;
	std::vector<uint8_t> late_flags;

	void Push(uint32_t orderkey, uint32_t suppkey, bool is_late) {
		orderkeys.push_back(orderkey);
		suppkeys.push_back(suppkey);
		late_flags.push_back(is_late ? 1 : 0);
	}

	size_t size() const {
		return orderkeys.size();
	}

	size_t LogicalBytes() const {
		return orderkeys.size() * sizeof(uint32_t) + suppkeys.size() * sizeof(uint32_t) +
		       late_flags.size() * sizeof(uint8_t);
	}

	size_t CapacityBytes() const {
		return orderkeys.capacity() * sizeof(uint32_t) + suppkeys.capacity() * sizeof(uint32_t) +
		       late_flags.capacity() * sizeof(uint8_t);
	}
};

class OrderKeyBitmap {
public:
	bool Contains(int64_t orderkey) const {
		if (orderkey < 0) {
			return false;
		}
		const auto key = static_cast<uint64_t>(orderkey);
		if (probe_unchecked) {
			return ContainsUnchecked(key);
		}
		const auto word_index = key >> 6;
		if (word_index >= words.size()) {
			return false;
		}
		const auto mask = uint64_t {1} << (key & 63);
		return (words[word_index] & mask) != 0;
	}

	void InsertUnchecked(int64_t orderkey) {
		if (orderkey < 0) {
			return;
		}
		const auto key = static_cast<uint64_t>(orderkey);
		const auto word_index = key >> 6;
		if (word_index >= words.size()) {
			EnsureCapacity(key);
		}
		words[word_index] |= uint64_t {1} << (key & 63);
	}

	void EnableUncheckedProbe(int64_t capacity_hint) {
		EnsureCapacity(static_cast<uint64_t>(std::max<int64_t>(capacity_hint - 1, 0)));
		probe_unchecked = true;
	}

private:
	bool ContainsUnchecked(uint64_t key) const {
		const auto mask = uint64_t {1} << (key & 63);
		return (words[key >> 6] & mask) != 0;
	}

	void EnsureCapacity(uint64_t max_bit_inclusive) {
		const auto needed_words = (max_bit_inclusive >> 6) + 1;
		if (needed_words <= words.size()) {
			return;
		}
		auto new_size = words.empty() ? needed_words : words.size();
		while (new_size < needed_words) {
			new_size *= 2;
		}
		words.resize(new_size, 0);
	}

	std::vector<uint64_t> words;
	bool probe_unchecked = false;
};

struct QualifyingSupplierIndex {
	std::vector<int64_t> compact_to_suppkey;
	std::vector<std::string> compact_names;
	std::unordered_map<int64_t, int32_t> suppkey_to_compact;

	int32_t compact_count() const {
		return static_cast<int32_t>(compact_to_suppkey.size());
	}

	bool IsQualifying(int64_t suppkey) const {
		return suppkey_to_compact.find(suppkey) != suppkey_to_compact.end();
	}

	bool IsQualifyingU32(uint32_t suppkey) const {
		return IsQualifying(static_cast<int64_t>(suppkey));
	}

	int32_t ToCompact(int64_t suppkey) const {
		return suppkey_to_compact.at(suppkey);
	}

	int32_t ToCompactU32(uint32_t suppkey) const {
		return ToCompact(static_cast<int64_t>(suppkey));
	}

	void Insert(int64_t suppkey, std::string name) {
		if (suppkey_to_compact.emplace(suppkey, compact_count()).second) {
			compact_to_suppkey.push_back(suppkey);
			compact_names.push_back(std::move(name));
		}
	}
};

struct Q21FrozenState {
	OrderKeyBitmap finished_orders;
	QualifyingSupplierIndex supplier_index;
	int64_t order_capacity = 0;
};

struct PartitionProfile {
	size_t record_count = 0;
	size_t touched_orders = 0;
	int64_t process_ns = 0;
};

struct Q21ParallelProfile {
	int64_t supplier_ns = 0;
	int64_t orders_ns = 0;
	int64_t lineitem_ns = 0;
	int64_t partition_finalize_ns = 0;
	int64_t wait_reduce_ns = 0;
	int64_t output_ns = 0;
	size_t worker_count = 0;
	size_t num_partitions = 0;
	size_t total_compact_records = 0;
	size_t total_order_groups = 0;
	size_t max_order_group_size = 0;
	size_t fragment_logical_bytes = 0;
	size_t fragment_capacity_bytes = 0;
	size_t compact_logical_bytes_per_record = 0;
	size_t compact_struct_size = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint8_t);
	int64_t order_capacity = 0;
	size_t min_partition_records = 0;
	size_t max_partition_records = 0;
	size_t min_partition_ns = 0;
	size_t max_partition_ns = 0;
	size_t median_partition_ns = 0;
};

bool BetterOutputRow(const Q21OutputRow &lhs, const Q21OutputRow &rhs) {
	if (lhs.numwait != rhs.numwait) {
		return lhs.numwait > rhs.numwait;
	}
	return lhs.name < rhs.name;
}

std::string ReadString(const duckdb::string_t &value) {
	return std::string(value.GetData(), value.GetData() + value.GetSize());
}

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

bool TryNarrowU32(int64_t value, uint32_t &out) {
	if (value < 0 || static_cast<uint64_t>(value) > std::numeric_limits<uint32_t>::max()) {
		return false;
	}
	out = static_cast<uint32_t>(value);
	return true;
}

uint32_t NarrowU32OrThrow(int64_t value, const char *label) {
	uint32_t out = 0;
	if (!TryNarrowU32(value, out)) {
		throw duckdb::InternalException("Q21 parallel %s out of uint32 range: %lld", label,
		                                static_cast<long long>(value));
	}
	return out;
}

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

size_t ChoosePartitionCount(duckdb::idx_t worker_count) {
	const auto target = static_cast<size_t>(worker_count) * kPartitionsPerWorker;
	return std::min(kMaxPartitions, std::max(kMinPartitions, target));
}

uint32_t PartitionId(uint32_t orderkey, size_t num_partitions, int64_t order_capacity) {
	if (num_partitions == 0) {
		return 0;
	}
	if (order_capacity <= 1) {
		return 0;
	}
	const auto capacity = static_cast<uint64_t>(order_capacity);
	const auto key = static_cast<uint64_t>(orderkey);
	if (key >= capacity) {
		return static_cast<uint32_t>(num_partitions - 1);
	}
	return static_cast<uint32_t>((key * num_partitions) / capacity);
}

void PartitionOrderkeyRange(size_t partition_id, size_t num_partitions, int64_t order_capacity, int64_t &min_orderkey,
                            int64_t &max_orderkey) {
	if (order_capacity <= 0) {
		min_orderkey = 0;
		max_orderkey = -1;
		return;
	}
	const auto capacity = static_cast<uint64_t>(order_capacity);
	const auto pid = static_cast<uint64_t>(partition_id);
	const auto parts = static_cast<uint64_t>(num_partitions);
	min_orderkey = static_cast<int64_t>((pid * capacity) / parts);
	max_orderkey = static_cast<int64_t>(((pid + 1) * capacity) / parts) - 1;
	if (max_orderkey < min_orderkey) {
		max_orderkey = min_orderkey - 1;
	}
}

void UpdateOrderSummary(OrderSummary &summary, uint32_t suppkey, bool is_late) {
	summary.line_count++;
	if (!(summary.flags & kInitialized)) {
		summary.first_supplier = suppkey;
		summary.flags |= kInitialized;
	} else if (suppkey != summary.first_supplier) {
		summary.flags |= kOtherSupplier;
	}
	if (!is_late) {
		return;
	}
	if (!(summary.flags & kLateSeen)) {
		summary.first_late_supplier = suppkey;
		summary.first_late_count = 1;
		summary.flags |= kLateSeen;
	} else if (suppkey == summary.first_late_supplier) {
		summary.first_late_count++;
	} else {
		summary.flags |= kOtherLateSupplier;
	}
}

bool OrderContributes(const OrderSummary &summary, const QualifyingSupplierIndex &supplier_index) {
	if (!(summary.flags & kOtherSupplier)) {
		return false;
	}
	if (!(summary.flags & kLateSeen)) {
		return false;
	}
	if (summary.flags & kOtherLateSupplier) {
		return false;
	}
	return supplier_index.IsQualifyingU32(summary.first_late_supplier);
}

void BuildSupplierState(duckdb::ClientContext &context, const duckdb::FunctionData &supplier_bind,
                        Q21FrozenState &state) {
	auto source = CreateNativeChunkSource(context, supplier_bind);
	while (source->Next()) {
		auto &chunk = source->Chunk();
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat name_format;
		chunk.data[Q21SupplierChunkLayout::SUPPKEY_INDEX].ToUnifiedFormat(chunk.size(), suppkey_format);
		chunk.data[Q21SupplierChunkLayout::NAME_INDEX].ToUnifiedFormat(chunk.size(), name_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index))) {
				continue;
			}
			const auto suppkey = suppkeys[suppkey_index];
			state.supplier_index.Insert(
			    suppkey, ReadString(duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index]));
		}
	}
}

void BuildOrdersState(duckdb::ClientContext &context, const duckdb::FunctionData &orders_bind, Q21FrozenState &state) {
	const auto worker_count = ParallelWorkerCount(context);
	std::vector<CacheLinePadded<std::vector<int64_t>>> workers(worker_count);
	RunParallelChunkScan(context, orders_bind, worker_count, [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
		auto &local = workers[worker_id].value;
		auto &orderkey_vector = chunk.data[Q21OrdersChunkLayout::ORDERKEY_INDEX];
		if (IsFlatAllValid(orderkey_vector)) {
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
			local.insert(local.end(), orderkeys, orderkeys + chunk.size());
			return;
		}
		duckdb::UnifiedVectorFormat orderkey_format;
		orderkey_vector.ToUnifiedFormat(chunk.size(), orderkey_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			if (!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) {
				continue;
			}
			local.push_back(orderkeys[orderkey_index]);
		}
	});
	for (const auto &worker : workers) {
		for (const auto orderkey : worker.value) {
			state.finished_orders.InsertUnchecked(orderkey);
		}
	}
	state.finished_orders.EnableUncheckedProbe(state.order_capacity);
}

struct ScanWorkerState {
	std::vector<CompactFragment> partitions;

	explicit ScanWorkerState(size_t num_partitions) : partitions(num_partitions) {
	}
};

void AccumulateLineitemChunk(duckdb::DataChunk &chunk, const Q21FrozenState &state, size_t num_partitions,
                             ScanWorkerState &worker_state) {
	auto &orderkey_vector = chunk.data[Q21LineitemChunkLayout::ORDERKEY_INDEX];
	auto &suppkey_vector = chunk.data[Q21LineitemChunkLayout::SUPPKEY_INDEX];
	auto &commitdate_vector = chunk.data[Q21LineitemChunkLayout::COMMITDATE_INDEX];
	auto &receiptdate_vector = chunk.data[Q21LineitemChunkLayout::RECEIPTDATE_INDEX];

	const auto push_row = [&](int64_t orderkey, int64_t suppkey, int32_t commitdate, int32_t receiptdate) {
		if (!state.finished_orders.Contains(orderkey)) {
			return;
		}
		const auto order_u32 = NarrowU32OrThrow(orderkey, "orderkey");
		const auto supp_u32 = NarrowU32OrThrow(suppkey, "suppkey");
		const auto partition = PartitionId(order_u32, num_partitions, state.order_capacity);
		worker_state.partitions[partition].Push(order_u32, supp_u32, receiptdate > commitdate);
	};

	if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(suppkey_vector) && IsFlatAllValid(commitdate_vector) &&
	    IsFlatAllValid(receiptdate_vector)) {
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
		auto commitdates = duckdb::FlatVector::GetData<int32_t>(commitdate_vector);
		auto receiptdates = duckdb::FlatVector::GetData<int32_t>(receiptdate_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			push_row(orderkeys[row], suppkeys[row], commitdates[row], receiptdates[row]);
		}
		return;
	}

	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat suppkey_format;
	duckdb::UnifiedVectorFormat commitdate_format;
	duckdb::UnifiedVectorFormat receiptdate_format;
	chunk.data[Q21LineitemChunkLayout::ORDERKEY_INDEX].ToUnifiedFormat(chunk.size(), orderkey_format);
	chunk.data[Q21LineitemChunkLayout::SUPPKEY_INDEX].ToUnifiedFormat(chunk.size(), suppkey_format);
	chunk.data[Q21LineitemChunkLayout::COMMITDATE_INDEX].ToUnifiedFormat(chunk.size(), commitdate_format);
	chunk.data[Q21LineitemChunkLayout::RECEIPTDATE_INDEX].ToUnifiedFormat(chunk.size(), receiptdate_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
	auto commitdates = duckdb::UnifiedVectorFormat::GetData<int32_t>(commitdate_format);
	auto receiptdates = duckdb::UnifiedVectorFormat::GetData<int32_t>(receiptdate_format);

	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto suppkey_index = suppkey_format.sel->get_index(row);
		const auto commitdate_index = commitdate_format.sel->get_index(row);
		const auto receiptdate_index = receiptdate_format.sel->get_index(row);
		if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
		    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
		    (!commitdate_format.validity.AllValid() && !commitdate_format.validity.RowIsValidUnsafe(commitdate_index)) ||
		    (!receiptdate_format.validity.AllValid() &&
		     !receiptdate_format.validity.RowIsValidUnsafe(receiptdate_index))) {
			continue;
		}
		push_row(orderkeys[orderkey_index], suppkeys[suppkey_index], commitdates[commitdate_index],
		         receiptdates[receiptdate_index]);
	}
}

void ConsumeFragmentIntoSummaries(const CompactFragment &fragment, int64_t partition_min, int64_t partition_max,
                                  std::vector<OrderSummary> &dense_summaries,
                                  std::unordered_map<uint32_t, OrderSummary> &sparse_summaries, bool use_dense,
                                  size_t &max_group_size) {
	for (size_t index = 0; index < fragment.size(); index++) {
		const auto orderkey = fragment.orderkeys[index];
		const auto suppkey = fragment.suppkeys[index];
		const auto is_late = fragment.late_flags[index] != 0;
		OrderSummary *summary = nullptr;
		if (use_dense) {
			if (orderkey < static_cast<uint32_t>(partition_min) ||
			    orderkey > static_cast<uint32_t>(partition_max)) {
				summary = &sparse_summaries[orderkey];
			} else {
				const auto offset = static_cast<size_t>(orderkey - static_cast<uint32_t>(partition_min));
				summary = &dense_summaries[offset];
			}
		} else {
			summary = &sparse_summaries[orderkey];
		}
		UpdateOrderSummary(*summary, suppkey, is_late);
		const auto group_size = static_cast<size_t>(summary->line_count);
		if (group_size > max_group_size) {
			max_group_size = group_size;
		}
	}
}

void EmitPartitionCounts(const std::vector<OrderSummary> &dense_summaries, int64_t partition_min, int64_t partition_max,
                         const QualifyingSupplierIndex &supplier_index, std::vector<int64_t> &local_counts,
                         size_t &touched_orders) {
	const auto span = static_cast<size_t>(partition_max - partition_min + 1);
	for (size_t offset = 0; offset < span; offset++) {
		const auto &summary = dense_summaries[offset];
		if (!(summary.flags & kInitialized)) {
			continue;
		}
		touched_orders++;
		if (!OrderContributes(summary, supplier_index)) {
			continue;
		}
		const auto compact = supplier_index.ToCompactU32(summary.first_late_supplier);
		local_counts[static_cast<size_t>(compact)] += summary.first_late_count;
	}
}

void EmitPartitionCountsSparse(const std::unordered_map<uint32_t, OrderSummary> &sparse_summaries,
                               const QualifyingSupplierIndex &supplier_index, std::vector<int64_t> &local_counts,
                               size_t &touched_orders) {
	for (const auto &entry : sparse_summaries) {
		const auto &summary = entry.second;
		if (!(summary.flags & kInitialized)) {
			continue;
		}
		touched_orders++;
		if (!OrderContributes(summary, supplier_index)) {
			continue;
		}
		const auto compact = supplier_index.ToCompactU32(summary.first_late_supplier);
		local_counts[static_cast<size_t>(compact)] += summary.first_late_count;
	}
}

PartitionProfile ProcessPartition(size_t partition_id, size_t num_partitions, const Q21FrozenState &state,
                                  const std::vector<ScanWorkerState> &workers, std::vector<int64_t> &local_counts,
                                  size_t &max_group_size) {
	using clock = std::chrono::steady_clock;
	const auto t0 = clock::now();

	PartitionProfile profile;
	int64_t partition_min = 0;
	int64_t partition_max = 0;
	PartitionOrderkeyRange(partition_id, num_partitions, state.order_capacity, partition_min, partition_max);
	const auto span = static_cast<size_t>(std::max<int64_t>(partition_max - partition_min + 1, 0));
	const bool use_dense = span > 0 && span <= kDensePartitionSpanLimit;

	std::vector<OrderSummary> dense_summaries;
	std::unordered_map<uint32_t, OrderSummary> sparse_summaries;
	if (use_dense) {
		dense_summaries.resize(span);
	}

	for (const auto &worker : workers) {
		const auto &fragment = worker.partitions[partition_id];
		profile.record_count += fragment.size();
		ConsumeFragmentIntoSummaries(fragment, partition_min, partition_max, dense_summaries, sparse_summaries,
		                             use_dense, max_group_size);
	}

	if (use_dense) {
		EmitPartitionCounts(dense_summaries, partition_min, partition_max, state.supplier_index, local_counts,
		                    profile.touched_orders);
	}
	if (!sparse_summaries.empty()) {
		EmitPartitionCountsSparse(sparse_summaries, state.supplier_index, local_counts, profile.touched_orders);
	}

	profile.process_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
	return profile;
}

void FinalizePartitionedWaitCounts(duckdb::ClientContext &context, const Q21FrozenState &state,
                                   const std::vector<ScanWorkerState> &workers, size_t num_partitions,
                                   std::vector<int64_t> &wait_count_by_compact, Q21ParallelProfile &profile) {
	using clock = std::chrono::steady_clock;
	const auto finalize_t0 = clock::now();

	const auto worker_count = ParallelWorkerCount(context);
	const auto compact_count = static_cast<size_t>(state.supplier_index.compact_count());
	std::vector<std::vector<int64_t>> partition_counts(num_partitions, std::vector<int64_t>(compact_count, 0));
	std::vector<PartitionProfile> partition_profiles(num_partitions);
	std::atomic<size_t> next_partition {0};
	std::atomic<size_t> global_max_group_size {0};

	duckdb::TaskExecutor executor(context);
	for (duckdb::idx_t worker_id = 0; worker_id < worker_count; worker_id++) {
		executor.ScheduleTask(duckdb::make_uniq<LambdaExecutorTask>(executor, [&, worker_id]() {
			(void)worker_id;
			while (true) {
				const auto partition_id = next_partition.fetch_add(1);
				if (partition_id >= num_partitions) {
					break;
				}
				size_t partition_max_group = 0;
				partition_profiles[partition_id] =
				    ProcessPartition(partition_id, num_partitions, state, workers, partition_counts[partition_id],
				                     partition_max_group);
				auto observed = global_max_group_size.load();
				while (partition_max_group > observed &&
				       !global_max_group_size.compare_exchange_weak(observed, partition_max_group)) {
				}
			}
		}));
	}
	executor.WorkOnTasks();
	if (context.interrupted) {
		throw duckdb::InterruptException();
	}

	profile.partition_finalize_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - finalize_t0).count();
	profile.max_order_group_size = global_max_group_size.load();

	const auto reduce_t0 = clock::now();
	wait_count_by_compact.assign(compact_count, 0);
	size_t total_order_groups = 0;
	std::vector<size_t> partition_records(num_partitions, 0);
	std::vector<int64_t> partition_ns(num_partitions, 0);
	for (size_t partition_id = 0; partition_id < num_partitions; partition_id++) {
		const auto &profile_entry = partition_profiles[partition_id];
		partition_records[partition_id] = profile_entry.record_count;
		partition_ns[partition_id] = profile_entry.process_ns;
		total_order_groups += profile_entry.touched_orders;
		for (size_t compact = 0; compact < compact_count; compact++) {
			wait_count_by_compact[compact] += partition_counts[partition_id][compact];
		}
	}
	profile.wait_reduce_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - reduce_t0).count();
	profile.total_order_groups = total_order_groups;

	if (!partition_records.empty()) {
		profile.min_partition_records =
		    *std::min_element(partition_records.begin(), partition_records.end());
		profile.max_partition_records =
		    *std::max_element(partition_records.begin(), partition_records.end());
	}
	if (!partition_ns.empty()) {
		profile.min_partition_ns = static_cast<size_t>(*std::min_element(partition_ns.begin(), partition_ns.end()));
		profile.max_partition_ns = static_cast<size_t>(*std::max_element(partition_ns.begin(), partition_ns.end()));
		auto sorted_ns = partition_ns;
		std::sort(sorted_ns.begin(), sorted_ns.end());
		profile.median_partition_ns = static_cast<size_t>(sorted_ns[sorted_ns.size() / 2]);
	}
}

void PrintQ21ParallelProfile(const Q21ParallelProfile &profile) {
	std::cerr << "q21_parallel_profile"
	          << " supplier_ns=" << profile.supplier_ns << " orders_ns=" << profile.orders_ns
	          << " lineitem_ns=" << profile.lineitem_ns << " partition_finalize_ns=" << profile.partition_finalize_ns
	          << " wait_reduce_ns=" << profile.wait_reduce_ns << " output_ns=" << profile.output_ns
	          << " worker_count=" << profile.worker_count << " num_partitions=" << profile.num_partitions
	          << " total_compact_records=" << profile.total_compact_records
	          << " total_order_groups=" << profile.total_order_groups
	          << " max_order_group_size=" << profile.max_order_group_size
	          << " fragment_logical_bytes=" << profile.fragment_logical_bytes
	          << " fragment_capacity_bytes=" << profile.fragment_capacity_bytes
	          << " compact_logical_bytes_per_record=" << profile.compact_logical_bytes_per_record
	          << " compact_struct_size=" << profile.compact_struct_size << " order_capacity=" << profile.order_capacity
	          << " min_partition_records=" << profile.min_partition_records
	          << " max_partition_records=" << profile.max_partition_records
	          << " min_partition_ns=" << profile.min_partition_ns << " max_partition_ns=" << profile.max_partition_ns
	          << " median_partition_ns=" << profile.median_partition_ns << "\n";
}

class Q21ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q21ParallelKernel(Q21Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 3 || !bound_inputs[0] || !bound_inputs[1] || !bound_inputs[2]) {
			throw duckdb::InternalException("Q21 parallel expects bound supplier, orders, lineitem scans");
		}

		using clock = std::chrono::steady_clock;
		Q21FrozenState frozen;
		frozen.order_capacity = EstimateTpchDenseOrderCapacity(context, *bound_inputs[1]);

		const auto supplier_t0 = clock::now();
		BuildSupplierState(context, *bound_inputs[0], frozen);
		const auto supplier_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - supplier_t0).count();

		const auto orders_t0 = clock::now();
		BuildOrdersState(context, *bound_inputs[1], frozen);
		const auto orders_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - orders_t0).count();

		const auto worker_count = ParallelWorkerCount(context);
		const auto num_partitions = ChoosePartitionCount(worker_count);
		std::vector<ScanWorkerState> workers(static_cast<size_t>(worker_count),
		                                     ScanWorkerState(num_partitions));

		const auto lineitem_t0 = clock::now();
		RunParallelChunkScan(context, *bound_inputs[2], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     AccumulateLineitemChunk(chunk, frozen, num_partitions,
			                                             workers[static_cast<size_t>(worker_id)]);
		                     });
		const auto lineitem_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - lineitem_t0).count();

		Q21ParallelProfile profile;
		profile.supplier_ns = supplier_ns;
		profile.orders_ns = orders_ns;
		profile.lineitem_ns = lineitem_ns;
		profile.worker_count = static_cast<size_t>(worker_count);
		profile.num_partitions = num_partitions;
		profile.order_capacity = frozen.order_capacity;
		profile.compact_struct_size = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint8_t);

		size_t total_compact_records = 0;
		size_t fragment_logical_bytes = 0;
		size_t fragment_capacity_bytes = 0;
		for (const auto &worker : workers) {
			for (const auto &fragment : worker.partitions) {
				total_compact_records += fragment.size();
				fragment_logical_bytes += fragment.LogicalBytes();
				fragment_capacity_bytes += fragment.CapacityBytes();
			}
		}
		profile.total_compact_records = total_compact_records;
		profile.fragment_logical_bytes = fragment_logical_bytes;
		profile.fragment_capacity_bytes = fragment_capacity_bytes;
		profile.compact_logical_bytes_per_record =
		    total_compact_records == 0 ? 0 : fragment_logical_bytes / total_compact_records;

		std::vector<int64_t> wait_count_by_compact;
		FinalizePartitionedWaitCounts(context, frozen, workers, num_partitions, wait_count_by_compact, profile);

		const auto output_t0 = clock::now();
		output_rows.clear();
		for (int32_t compact = 0; compact < frozen.supplier_index.compact_count(); compact++) {
			const auto count = wait_count_by_compact[static_cast<size_t>(compact)];
			if (count == 0) {
				continue;
			}
			Q21OutputRow row;
			row.name = frozen.supplier_index.compact_names[static_cast<size_t>(compact)];
			row.numwait = count;
			output_rows.push_back(std::move(row));
		}
		std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
		profile.output_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - output_t0).count();

		if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
			PrintQ21ParallelProfile(profile);
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
			output.SetValue(0, row, duckdb::Value(entry.name));
			output.SetValue(1, row, duckdb::Value::BIGINT(entry.numwait));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	Q21Parameters params;
	std::vector<Q21OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ21ParallelKernel(const Q21Parameters &params) {
	return duckdb::make_uniq<Q21ParallelKernel>(params);
}

} // namespace accelerator_codegen
