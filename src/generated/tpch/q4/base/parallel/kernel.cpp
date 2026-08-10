#include "accelerator_codegen/runtime/parallel/parallel_kernel.hpp"
#include "tpch/q4/base/query.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
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

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr int kPrioritySlotCount = 5;

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

class QualifyingOrderKeySet {
public:
	QualifyingOrderKeySet() {
		EnsureCapacity(6000000);
	}

	void Insert(int64_t orderkey) {
		if (orderkey < 0 || orderkey > 1'000'000'000) {
			overflow.insert(orderkey);
			return;
		}
		const auto key = static_cast<uint64_t>(orderkey);
		if ((key >> 6) >= words.size()) {
			EnsureCapacity(key);
		}
		words[key >> 6] |= (uint64_t {1} << (key & 63));
	}

	bool Contains(int64_t orderkey) const {
		if (orderkey < 0) {
			return overflow.find(orderkey) != overflow.end();
		}
		const auto key = static_cast<uint64_t>(orderkey);
		const auto word_index = key >> 6;
		if (word_index >= words.size()) {
			return overflow.find(orderkey) != overflow.end();
		}
		if ((words[word_index] & (uint64_t {1} << (key & 63))) != 0) {
			return true;
		}
		return overflow.find(orderkey) != overflow.end();
	}

	void OrFrom(const QualifyingOrderKeySet &other) {
		if (other.words.size() > words.size()) {
			words.resize(other.words.size(), 0);
		}
		for (size_t i = 0; i < other.words.size(); i++) {
			words[i] |= other.words[i];
		}
		overflow.insert(other.overflow.begin(), other.overflow.end());
	}

	//! Bitmap word storage bytes (excludes overflow set).
	size_t BitmapBytes() const {
		return words.size() * sizeof(uint64_t);
	}

private:
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
	std::unordered_set<int64_t> overflow;
};

struct Q4OutputRow {
	std::string orderpriority;
	int64_t order_count = 0;
};

const char *kKnownPriorities[kPrioritySlotCount] = {"1-URGENT", "2-HIGH", "3-MEDIUM", "4-NOT SPECIFIED", "5-LOW"};

int MatchFixedPriority(const duckdb::string_t &value) {
	const auto size = value.GetSize();
	const auto data = value.GetData();
	for (int slot = 0; slot < kPrioritySlotCount; slot++) {
		const auto *known = kKnownPriorities[slot];
		const auto known_size = std::strlen(known);
		if (size == known_size && std::memcmp(data, known, known_size) == 0) {
			return slot;
		}
	}
	return -1;
}

struct WorkerPriorityAgg {
	int64_t fixed_priority_counts[kPrioritySlotCount] = {};
	std::unordered_map<std::string, int64_t> generic_priority_counts;
};

class Q4ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q4ParallelKernel(Q4Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 2 || !bound_inputs[0] || !bound_inputs[1]) {
			throw duckdb::InternalException("Q4 parallel expects lineitem + orders");
		}

		using clock = std::chrono::steady_clock;
		const auto worker_count = ParallelWorkerCount(context);
		profile_worker_count = worker_count;
		std::vector<CacheLinePadded<QualifyingOrderKeySet>> lineitem_workers(worker_count);

		const auto lineitem_t0 = clock::now();
		RunParallelChunkScan(context, *bound_inputs[0], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &input) {
			                     auto &local = lineitem_workers[worker_id].value;
			                     auto &orderkey_vector = input.data[Q4LineitemChunkLayout::ORDERKEY_INDEX];
			                     auto &commitdate_vector = input.data[Q4LineitemChunkLayout::COMMITDATE_INDEX];
			                     auto &receiptdate_vector = input.data[Q4LineitemChunkLayout::RECEIPTDATE_INDEX];

			                     if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(commitdate_vector) &&
			                         IsFlatAllValid(receiptdate_vector)) {
				                     auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
				                     auto commitdates = duckdb::FlatVector::GetData<duckdb::date_t>(commitdate_vector);
				                     auto receiptdates =
				                         duckdb::FlatVector::GetData<duckdb::date_t>(receiptdate_vector);
				                     for (duckdb::idx_t row = 0; row < input.size(); row++) {
					                     if (commitdates[row] < receiptdates[row]) {
						                     local.Insert(orderkeys[row]);
					                     }
				                     }
				                     return;
			                     }

			                     duckdb::UnifiedVectorFormat orderkey_format;
			                     duckdb::UnifiedVectorFormat commitdate_format;
			                     duckdb::UnifiedVectorFormat receiptdate_format;
			                     orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
			                     commitdate_vector.ToUnifiedFormat(input.size(), commitdate_format);
			                     receiptdate_vector.ToUnifiedFormat(input.size(), receiptdate_format);
			                     auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
			                     auto commitdates =
			                         duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(commitdate_format);
			                     auto receiptdates =
			                         duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(receiptdate_format);
			                     for (duckdb::idx_t row = 0; row < input.size(); row++) {
				                     const auto orderkey_index = orderkey_format.sel->get_index(row);
				                     const auto commit_index = commitdate_format.sel->get_index(row);
				                     const auto receipt_index = receiptdate_format.sel->get_index(row);
				                     if ((!orderkey_format.validity.AllValid() &&
				                          !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
				                         (!commitdate_format.validity.AllValid() &&
				                          !commitdate_format.validity.RowIsValidUnsafe(commit_index)) ||
				                         (!receiptdate_format.validity.AllValid() &&
				                          !receiptdate_format.validity.RowIsValidUnsafe(receipt_index))) {
					                     continue;
				                     }
				                     if (commitdates[commit_index] < receiptdates[receipt_index]) {
					                     local.Insert(orderkeys[orderkey_index]);
				                     }
			                     }
		                     });
		profile_lineitem_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - lineitem_t0).count();

		size_t max_bitmap_bytes = 0;
		for (const auto &w : lineitem_workers) {
			max_bitmap_bytes = std::max(max_bitmap_bytes, w.value.BitmapBytes());
		}
		profile_bitmap_bytes_per_worker = max_bitmap_bytes;

		const auto or_t0 = clock::now();
		QualifyingOrderKeySet qualifying_orderkeys;
		for (const auto &w : lineitem_workers) {
			qualifying_orderkeys.OrFrom(w.value);
		}
		profile_or_reduce_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - or_t0).count();

		std::vector<CacheLinePadded<WorkerPriorityAgg>> order_workers(worker_count);
		const auto orders_t0 = clock::now();
		RunParallelChunkScan(context, *bound_inputs[1], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &input) {
			                     auto &agg = order_workers[worker_id].value;
			                     auto increment_priority = [&](const duckdb::string_t &priority) {
				                     const auto slot = MatchFixedPriority(priority);
				                     if (slot >= 0) {
					                     agg.fixed_priority_counts[slot] += 1;
					                     return;
				                     }
				                     agg.generic_priority_counts[priority.GetString()] += 1;
			                     };

			                     if (IsFlatAllValid(input.data[Q4OrdersChunkLayout::ORDERDATE_INDEX]) &&
			                         IsFlatAllValid(input.data[Q4OrdersChunkLayout::ORDERKEY_INDEX]) &&
			                         IsFlatAllValid(input.data[Q4OrdersChunkLayout::ORDERPRIORITY_INDEX])) {
				                     auto orderdates = duckdb::FlatVector::GetData<duckdb::date_t>(
				                         input.data[Q4OrdersChunkLayout::ORDERDATE_INDEX]);
				                     auto orderkeys = duckdb::FlatVector::GetData<int64_t>(
				                         input.data[Q4OrdersChunkLayout::ORDERKEY_INDEX]);
				                     auto priorities = duckdb::FlatVector::GetData<duckdb::string_t>(
				                         input.data[Q4OrdersChunkLayout::ORDERPRIORITY_INDEX]);
				                     for (duckdb::idx_t row = 0; row < input.size(); row++) {
					                     if (!(orderdates[row] >= params.start_date &&
					                           orderdates[row] < params.end_date)) {
						                     continue;
					                     }
					                     if (!qualifying_orderkeys.Contains(orderkeys[row])) {
						                     continue;
					                     }
					                     increment_priority(priorities[row]);
				                     }
				                     return;
			                     }

			                     duckdb::UnifiedVectorFormat orderdate_format;
			                     duckdb::UnifiedVectorFormat orderkey_format;
			                     duckdb::UnifiedVectorFormat priority_format;
			                     input.data[Q4OrdersChunkLayout::ORDERDATE_INDEX].ToUnifiedFormat(input.size(),
			                                                                                      orderdate_format);
			                     input.data[Q4OrdersChunkLayout::ORDERKEY_INDEX].ToUnifiedFormat(input.size(),
			                                                                                     orderkey_format);
			                     input.data[Q4OrdersChunkLayout::ORDERPRIORITY_INDEX].ToUnifiedFormat(
			                         input.size(), priority_format);
			                     auto orderdates =
			                         duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(orderdate_format);
			                     auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
			                     auto priorities =
			                         duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(priority_format);
			                     for (duckdb::idx_t row = 0; row < input.size(); row++) {
				                     const auto orderdate_index = orderdate_format.sel->get_index(row);
				                     const auto orderkey_index = orderkey_format.sel->get_index(row);
				                     const auto priority_index = priority_format.sel->get_index(row);
				                     if ((!orderdate_format.validity.AllValid() &&
				                          !orderdate_format.validity.RowIsValidUnsafe(orderdate_index)) ||
				                         (!orderkey_format.validity.AllValid() &&
				                          !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
				                         (!priority_format.validity.AllValid() &&
				                          !priority_format.validity.RowIsValidUnsafe(priority_index))) {
					                     continue;
				                     }
				                     const auto orderdate = orderdates[orderdate_index];
				                     if (!(orderdate >= params.start_date && orderdate < params.end_date)) {
					                     continue;
				                     }
				                     if (!qualifying_orderkeys.Contains(orderkeys[orderkey_index])) {
					                     continue;
				                     }
				                     increment_priority(priorities[priority_index]);
			                     }
		                     });
		profile_orders_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - orders_t0).count();

		const auto merge_t0 = clock::now();
		for (const auto &w : order_workers) {
			for (int slot = 0; slot < kPrioritySlotCount; slot++) {
				fixed_priority_counts[slot] += w.value.fixed_priority_counts[slot];
			}
			for (const auto &entry : w.value.generic_priority_counts) {
				generic_priority_counts[entry.first] += entry.second;
			}
		}
		profile_merge_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - merge_t0).count();

		// Coarse phase profile for Phase 9 audit (ACCEL_PARALLEL_PROFILE=1).
		// Fields: lineitem_ns,or_reduce_ns,orders_ns,merge_ns,bitmap_bytes_per_worker,worker_count
		MaybeEmitProfile();
	}

	void MaybeEmitProfile() const {
		const char *env = std::getenv("ACCEL_PARALLEL_PROFILE");
		if (!env || std::string(env) != "1") {
			return;
		}
		std::cerr << "q4_parallel_profile lineitem_ns=" << profile_lineitem_ns
		          << " or_reduce_ns=" << profile_or_reduce_ns << " orders_ns=" << profile_orders_ns
		          << " merge_ns=" << profile_merge_ns
		          << " bitmap_bytes_per_worker=" << profile_bitmap_bytes_per_worker
		          << " worker_count=" << profile_worker_count << "\n";
	}

	void Finalize() override {
		if (finalized) {
			return;
		}
		ordered_rows.clear();
		for (int slot = 0; slot < kPrioritySlotCount; slot++) {
			if (fixed_priority_counts[slot] == 0) {
				continue;
			}
			Q4OutputRow row;
			row.orderpriority = kKnownPriorities[slot];
			row.order_count = fixed_priority_counts[slot];
			ordered_rows.push_back(std::move(row));
		}
		for (const auto &entry : generic_priority_counts) {
			if (entry.second == 0) {
				continue;
			}
			Q4OutputRow row;
			row.orderpriority = entry.first;
			row.order_count = entry.second;
			ordered_rows.push_back(std::move(row));
		}
		std::sort(ordered_rows.begin(), ordered_rows.end(),
		          [](const Q4OutputRow &lhs, const Q4OutputRow &rhs) { return lhs.orderpriority < rhs.orderpriority; });
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
			output.SetValue(0, row, duckdb::Value(result.orderpriority));
			output.SetValue(1, row, duckdb::Value::BIGINT(result.order_count));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	Q4Parameters params;
	int64_t fixed_priority_counts[kPrioritySlotCount] = {};
	std::unordered_map<std::string, int64_t> generic_priority_counts;
	std::vector<Q4OutputRow> ordered_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
	int64_t profile_lineitem_ns = 0;
	int64_t profile_or_reduce_ns = 0;
	int64_t profile_orders_ns = 0;
	int64_t profile_merge_ns = 0;
	size_t profile_bitmap_bytes_per_worker = 0;
	duckdb::idx_t profile_worker_count = 0;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ4ParallelKernel(const Q4Parameters &params) {
	return duckdb::make_uniq<Q4ParallelKernel>(params);
}

} // namespace accelerator_codegen
