#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_kernel.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_runtime.hpp"
#include "tpch/q15/base/query.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
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
constexpr int64_t kDefaultSupplierCapacity = 10001;

#if defined(__SIZEOF_INT128__)
using q15_acc_t = __int128_t;

duckdb::hugeint_t FromAccumulator(q15_acc_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}

void AddRevenue(q15_acc_t &accumulator, int64_t extendedprice_raw, int64_t discount_raw) {
	accumulator += static_cast<q15_acc_t>(extendedprice_raw) * static_cast<q15_acc_t>(100 - discount_raw);
}
#else
using q15_acc_t = duckdb::hugeint_t;

duckdb::hugeint_t FromAccumulator(const q15_acc_t &value) {
	return value;
}

void AddRevenue(q15_acc_t &accumulator, int64_t extendedprice_raw, int64_t discount_raw) {
	duckdb::hugeint_t price_value;
	duckdb::hugeint_t factor_value;
	duckdb::hugeint_t product_value;
	if (!duckdb::Hugeint::TryConvert(extendedprice_raw, price_value) ||
	    !duckdb::Hugeint::TryConvert(static_cast<int64_t>(100 - discount_raw), factor_value) ||
	    !duckdb::Hugeint::TryMultiply(price_value, factor_value, product_value) ||
	    !duckdb::Hugeint::TryAddInPlace(accumulator, product_value)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q15 revenue");
	}
}
#endif

struct Q15OutputRow {
	int64_t suppkey = 0;
	std::string name;
	std::string address;
	std::string phone;
	duckdb::hugeint_t revenue;
};

bool BetterOutputRow(const Q15OutputRow &lhs, const Q15OutputRow &rhs) {
	return lhs.suppkey < rhs.suppkey;
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

std::string ReadString(const duckdb::string_t &value) {
	return std::string(value.GetData(), value.GetData() + value.GetSize());
}

class SupplierRevenueStore {
public:
	void Initialize(int64_t supplier_capacity) {
		const auto target = static_cast<size_t>(std::max(supplier_capacity, kDefaultSupplierCapacity));
		revenue.assign(target, 0);
		has_revenue.assign(target, 0);
		capacity = static_cast<int64_t>(target);
		active_suppkeys.clear();
		active_suppkeys.reserve(4096);
	}

	void EnsureCapacity(int64_t suppkey) {
		if (suppkey < 0) {
			return;
		}
		const auto required = static_cast<size_t>(suppkey) + 1;
		if (static_cast<int64_t>(required) <= capacity) {
			return;
		}
		size_t target = required;
		if (capacity > 0) {
			const auto doubled = static_cast<size_t>(capacity) * 2;
			if (doubled > target) {
				target = doubled;
			}
		}
		revenue.resize(target, 0);
		has_revenue.resize(target, 0);
		capacity = static_cast<int64_t>(target);
	}

	void Add(int64_t suppkey, int64_t extendedprice_raw, int64_t discount_raw) {
		if (suppkey < 0) {
			return;
		}
		EnsureCapacity(suppkey);
		const auto index = static_cast<size_t>(suppkey);
		if (!has_revenue[index]) {
			has_revenue[index] = 1;
			active_suppkeys.push_back(suppkey);
		}
		AddRevenue(revenue[index], extendedprice_raw, discount_raw);
	}

	void MergeActiveFrom(const SupplierRevenueStore &other) {
		for (const auto suppkey : other.active_suppkeys) {
			if (suppkey < 0 || suppkey >= other.capacity) {
				continue;
			}
			const auto index = static_cast<size_t>(suppkey);
			if (!other.has_revenue[index]) {
				continue;
			}
			EnsureCapacity(suppkey);
			const auto merged_index = static_cast<size_t>(suppkey);
			if (!has_revenue[merged_index]) {
				has_revenue[merged_index] = 1;
				active_suppkeys.push_back(suppkey);
			}
#if defined(__SIZEOF_INT128__)
			revenue[merged_index] += other.revenue[index];
#else
			if (!duckdb::Hugeint::TryAddInPlace(revenue[merged_index], other.revenue[index])) {
				throw duckdb::OutOfRangeException("Overflow while merging Q15 revenue");
			}
#endif
		}
	}

	void MergeActiveRange(const std::vector<CacheLinePadded<SupplierRevenueStore>> &workers, int64_t range_begin,
	                      int64_t range_end, std::vector<int64_t> &range_active) {
		for (const auto &worker : workers) {
			for (const auto suppkey : worker.value.active_suppkeys) {
				if (suppkey < range_begin || suppkey >= range_end) {
					continue;
				}
				if (suppkey < 0 || suppkey >= worker.value.capacity) {
					continue;
				}
				const auto index = static_cast<size_t>(suppkey);
				if (!worker.value.has_revenue[index]) {
					continue;
				}
				const auto merged_index = static_cast<size_t>(suppkey);
				if (!has_revenue[merged_index]) {
					has_revenue[merged_index] = 1;
					range_active.push_back(suppkey);
				}
#if defined(__SIZEOF_INT128__)
				revenue[merged_index] += worker.value.revenue[index];
#else
				if (!duckdb::Hugeint::TryAddInPlace(revenue[merged_index], worker.value.revenue[index])) {
					throw duckdb::OutOfRangeException("Overflow while merging Q15 revenue");
				}
#endif
			}
		}
	}

	int64_t capacity = 0;
	std::vector<q15_acc_t> revenue;
	std::vector<uint8_t> has_revenue;
	std::vector<int64_t> active_suppkeys;
};

void MergeWorkerRevenueStores(const std::vector<CacheLinePadded<SupplierRevenueStore>> &workers,
                              SupplierRevenueStore &merged, int64_t merged_capacity, duckdb::idx_t merge_workers) {
	if (workers.empty()) {
		return;
	}
	merged.Initialize(merged_capacity);
	if (workers.size() == 1) {
		merged.MergeActiveFrom(workers[0].value);
		return;
	}
	if (merge_workers <= 1) {
		for (const auto &worker : workers) {
			merged.MergeActiveFrom(worker.value);
		}
		return;
	}

	const auto workers_for_merge = std::min<duckdb::idx_t>(merge_workers, static_cast<duckdb::idx_t>(merged.capacity));
	const auto range_size = (static_cast<size_t>(merged.capacity) + workers_for_merge - 1) / workers_for_merge;
	std::vector<std::thread> threads;
	threads.reserve(workers_for_merge > 0 ? workers_for_merge - 1 : 0);
	std::vector<std::vector<int64_t>> range_actives(workers_for_merge);
	std::mutex error_mutex;
	std::exception_ptr error;

	auto run_range = [&](duckdb::idx_t range_id) {
		try {
			const auto range_begin = static_cast<int64_t>(range_id * range_size);
			const auto range_end = std::min<int64_t>(merged.capacity, static_cast<int64_t>((range_id + 1) * range_size));
			if (range_begin >= range_end) {
				return;
			}
			merged.MergeActiveRange(workers, range_begin, range_end, range_actives[range_id]);
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
		merged.active_suppkeys.insert(merged.active_suppkeys.end(), active.begin(), active.end());
	}
}

int64_t EstimateTpchMaxSuppkey(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
	NativeParallelScanHandle handle;
	handle.Initialize(context, bind_data);
	return static_cast<int64_t>(handle.storage->GetTotalRows());
}

void AccumulateLineitemRow(SupplierRevenueStore &revenue, int64_t suppkey, int64_t extendedprice_raw,
                           int64_t discount_raw) {
	if (suppkey < 0) {
		return;
	}
	revenue.Add(suppkey, extendedprice_raw, discount_raw);
}

void AccumulateLineitemChunk(duckdb::DataChunk &chunk, SupplierRevenueStore &revenue) {
	auto &suppkey_vector = chunk.data[Q15LineitemChunkLayout::SUPPKEY_INDEX];
	auto &extendedprice_vector = chunk.data[Q15LineitemChunkLayout::EXTENDEDPRICE_INDEX];
	auto &discount_vector = chunk.data[Q15LineitemChunkLayout::DISCOUNT_INDEX];

	if (IsFlatAllValid(suppkey_vector) && IsFlatAllValid(extendedprice_vector) && IsFlatAllValid(discount_vector)) {
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
		auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vector);
		auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			AccumulateLineitemRow(revenue, suppkeys[row], extendedprices[row], discounts[row]);
		}
		return;
	}

	if (IsDictionaryOverFlat(suppkey_vector) && IsDictionaryOverFlat(extendedprice_vector) &&
	    IsDictionaryOverFlat(discount_vector)) {
		auto &suppkey_sel = duckdb::DictionaryVector::SelVector(suppkey_vector);
		auto &extendedprice_sel = duckdb::DictionaryVector::SelVector(extendedprice_vector);
		auto &discount_sel = duckdb::DictionaryVector::SelVector(discount_vector);
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(suppkey_vector));
		auto extendedprices =
		    duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(extendedprice_vector));
		auto discounts = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(discount_vector));
		auto &suppkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(suppkey_vector));
		auto &extendedprice_validity =
		    duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(extendedprice_vector));
		auto &discount_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(discount_vector));
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto suppkey_index = suppkey_sel.get_index(row);
			const auto extendedprice_index = extendedprice_sel.get_index(row);
			const auto discount_index = discount_sel.get_index(row);
			if (!suppkey_validity.RowIsValid(suppkey_index) || !extendedprice_validity.RowIsValid(extendedprice_index) ||
			    !discount_validity.RowIsValid(discount_index)) {
				continue;
			}
			AccumulateLineitemRow(revenue, suppkeys[suppkey_index], extendedprices[extendedprice_index],
			                      discounts[discount_index]);
		}
		return;
	}

	duckdb::UnifiedVectorFormat suppkey_format;
	duckdb::UnifiedVectorFormat extendedprice_format;
	duckdb::UnifiedVectorFormat discount_format;
	suppkey_vector.ToUnifiedFormat(chunk.size(), suppkey_format);
	extendedprice_vector.ToUnifiedFormat(chunk.size(), extendedprice_format);
	discount_vector.ToUnifiedFormat(chunk.size(), discount_format);
	auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
	auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
	auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);

	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto suppkey_index = suppkey_format.sel->get_index(row);
		const auto extendedprice_index = extendedprice_format.sel->get_index(row);
		const auto discount_index = discount_format.sel->get_index(row);
		if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
		    (!extendedprice_format.validity.AllValid() &&
		     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
		    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index))) {
			continue;
		}
		AccumulateLineitemRow(revenue, suppkeys[suppkey_index], extendedprices[extendedprice_index],
		                      discounts[discount_index]);
	}
}

void FetchWinningSuppliers(duckdb::ClientContext &context, const duckdb::FunctionData &supplier_bind,
                           const std::unordered_set<int64_t> &winners, duckdb::hugeint_t max_revenue,
                           std::vector<Q15OutputRow> &output_rows) {
	auto source = CreateNativeChunkSource(context, supplier_bind);
	while (source->Next()) {
		auto &input = source->Chunk();
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat name_format;
		duckdb::UnifiedVectorFormat address_format;
		duckdb::UnifiedVectorFormat phone_format;
		input.data[Q15SupplierChunkLayout::SUPPKEY_INDEX].ToUnifiedFormat(input.size(), suppkey_format);
		input.data[Q15SupplierChunkLayout::NAME_INDEX].ToUnifiedFormat(input.size(), name_format);
		input.data[Q15SupplierChunkLayout::ADDRESS_INDEX].ToUnifiedFormat(input.size(), address_format);
		input.data[Q15SupplierChunkLayout::PHONE_INDEX].ToUnifiedFormat(input.size(), phone_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			const auto address_index = address_format.sel->get_index(row);
			const auto phone_index = phone_format.sel->get_index(row);
			if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index)) ||
			    (!address_format.validity.AllValid() && !address_format.validity.RowIsValidUnsafe(address_index)) ||
			    (!phone_format.validity.AllValid() && !phone_format.validity.RowIsValidUnsafe(phone_index))) {
				continue;
			}
			const auto suppkey = suppkeys[suppkey_index];
			if (winners.find(suppkey) == winners.end()) {
				continue;
			}
			Q15OutputRow row_out;
			row_out.suppkey = suppkey;
			row_out.name = ReadString(
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index]);
			row_out.address = ReadString(
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(address_format)[address_index]);
			row_out.phone = ReadString(
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(phone_format)[phone_index]);
			row_out.revenue = max_revenue;
			output_rows.push_back(std::move(row_out));
		}
	}
}

class Q15ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q15ParallelKernel(Q15Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 2 || !bound_inputs[0] || !bound_inputs[1]) {
			throw duckdb::InternalException("Q15 parallel expects bound lineitem and supplier scans");
		}

		using clock = std::chrono::steady_clock;
		const auto worker_count = ParallelWorkerCount(context);
		const auto supplier_capacity = EstimateTpchMaxSuppkey(context, *bound_inputs[1]) + 1;
		std::vector<CacheLinePadded<SupplierRevenueStore>> workers(worker_count);

		const auto lineitem_t0 = clock::now();
		RunParallelChunkScan(context, *bound_inputs[0], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     AccumulateLineitemChunk(chunk, workers[worker_id].value);
		                     });
		const auto lineitem_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - lineitem_t0).count();

		const auto merge_t0 = clock::now();
		int64_t merged_capacity = supplier_capacity;
		for (const auto &worker : workers) {
			merged_capacity = std::max(merged_capacity, worker.value.capacity);
		}
		SupplierRevenueStore merged;
		MergeWorkerRevenueStores(workers, merged, merged_capacity, worker_count);
		const auto merge_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - merge_t0).count();

		duckdb::hugeint_t max_revenue;
		bool has_max_revenue = false;
		for (const auto suppkey : merged.active_suppkeys) {
			if (suppkey < 0 || suppkey >= merged.capacity) {
				continue;
			}
			const auto index = static_cast<size_t>(suppkey);
			if (!merged.has_revenue[index]) {
				continue;
			}
			const auto revenue = FromAccumulator(merged.revenue[index]);
			if (!has_max_revenue || duckdb::Hugeint::GreaterThan(revenue, max_revenue)) {
				max_revenue = revenue;
				has_max_revenue = true;
			}
		}

		output_rows.clear();
		if (has_max_revenue) {
			std::unordered_set<int64_t> winners;
			for (const auto suppkey : merged.active_suppkeys) {
				if (suppkey < 0 || suppkey >= merged.capacity) {
					continue;
				}
				const auto index = static_cast<size_t>(suppkey);
				if (!merged.has_revenue[index]) {
					continue;
				}
				if (duckdb::Hugeint::Equals(FromAccumulator(merged.revenue[index]), max_revenue)) {
					winners.insert(suppkey);
				}
			}
			const auto supplier_t0 = clock::now();
			FetchWinningSuppliers(context, *bound_inputs[1], winners, max_revenue, output_rows);
			const auto supplier_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - supplier_t0).count();
			std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);

			if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
				std::cerr << "q15_parallel_profile lineitem_ns=" << lineitem_ns << " merge_ns=" << merge_ns
				          << " supplier_ns=" << supplier_ns << " worker_count=" << worker_count
				          << " winners=" << winners.size() << "\n";
			}
		} else if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
			std::cerr << "q15_parallel_profile lineitem_ns=" << lineitem_ns << " merge_ns=" << merge_ns
			          << " supplier_ns=0 worker_count=" << worker_count << " winners=0\n";
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
			output.SetValue(0, row, duckdb::Value::BIGINT(entry.suppkey));
			output.SetValue(1, row, duckdb::Value(entry.name));
			output.SetValue(2, row, duckdb::Value(entry.address));
			output.SetValue(3, row, duckdb::Value(entry.phone));
			output.SetValue(4, row, duckdb::Value::DECIMAL(entry.revenue, kRevenueDecimalWidth, kRevenueDecimalScale));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	Q15Parameters params;
	std::vector<Q15OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ15ParallelKernel(const Q15Parameters &params) {
	return duckdb::make_uniq<Q15ParallelKernel>(params);
}

} // namespace accelerator_codegen
