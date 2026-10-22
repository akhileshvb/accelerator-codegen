#include "backends/duckdb/api/duckdb_scan_adapter.hpp"
#include "backends/duckdb/api/parallel/parallel_kernel.hpp"
#include "backends/duckdb/api/parallel/parallel_runtime.hpp"
#include "tpch/q15/duckdb/base/q15_revenue.hpp"
#include "tpch/q15/duckdb/base/query.hpp"

#include <algorithm>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;
constexpr duckdb::idx_t kOutputBatchSize = 64;

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

std::string ReadString(const duckdb::string_t &value) {
	return std::string(value.GetData(), value.GetData() + value.GetSize());
}

//! Run fn(task) for task in [0, task_count) on task_count threads (the caller runs task 0).
template <typename Fn>
void RunTasks(duckdb::idx_t task_count, Fn fn) {
	if (task_count <= 1) {
		fn(0);
		return;
	}
	std::mutex error_mutex;
	std::exception_ptr error;
	auto run = [&](duckdb::idx_t task) {
		try {
			fn(task);
		} catch (...) {
			std::lock_guard<std::mutex> lock(error_mutex);
			if (!error) {
				error = std::current_exception();
			}
		}
	};
	std::vector<std::thread> threads;
	threads.reserve(task_count - 1);
	for (duckdb::idx_t task = 1; task < task_count; task++) {
		threads.emplace_back(run, task);
	}
	run(0);
	for (auto &thread : threads) {
		thread.join();
	}
	if (error) {
		std::rethrow_exception(error);
	}
}

//! Appends the supplier rows whose key is in winners (sorted) to out.
void CollectWinningSuppliers(duckdb::DataChunk &input, const std::vector<int64_t> &winners,
                             duckdb::hugeint_t max_revenue, std::vector<Q15OutputRow> &output_rows) {
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
		if (!std::binary_search(winners.begin(), winners.end(), suppkey)) {
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

//! Winners are few (usually one): push s_suppkey BETWEEN min AND max into the supplier scan so zone maps
//! skip the other segments and only matching rows decode the strings.  CollectWinningSuppliers re-checks
//! exact membership.
void FetchWinningSuppliers(duckdb::ClientContext &context, const duckdb::FunctionData &supplier_bind,
                           const std::vector<int64_t> &winners, duckdb::hugeint_t max_revenue,
                           std::vector<Q15OutputRow> &output_rows) {
	duckdb::TableFilterSet key_range;
	const duckdb::ColumnIndex suppkey_column(Q15SupplierChunkLayout::SUPPKEY_INDEX);
	key_range.PushFilter(suppkey_column, duckdb::make_uniq<duckdb::ConstantFilter>(
	                                         duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
	                                         duckdb::Value::BIGINT(winners.front())));
	key_range.PushFilter(suppkey_column,
	                     duckdb::make_uniq<duckdb::ConstantFilter>(duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO,
	                                                               duckdb::Value::BIGINT(winners.back())));
	auto source = CreateNativeChunkSource(context, supplier_bind, &key_range);
	while (source->Next()) {
		CollectWinningSuppliers(source->Chunk(), winners, max_revenue, output_rows);
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
		const auto worker_count = ParallelWorkerCount(context);
		std::vector<CacheLinePadded<q15::ChunkAggregator>> workers(worker_count);
		const auto supplier_capacity = static_cast<size_t>(GetNativeScanTableRowCount(context, *bound_inputs[1])) + 1;

		RunParallelChunkScan(context, *bound_inputs[0], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     auto &worker = workers[worker_id].value;
			                     worker.Store().Reserve(supplier_capacity);
			                     worker.Consume(chunk, params);
		                     });

		// Parallel merge: each task owns a slice of the dense suppkey range, sums it across all worker
		// stores and keeps the slice maximum with its keys.  Integer sums, so the order cannot matter.
		size_t capacity = 0;
		for (const auto &worker : workers) {
			capacity = std::max(capacity, worker.value.Store().Capacity());
		}
		struct SliceMax {
			bool has = false;
			q15::Acc revenue {};
			std::vector<int64_t> keys;
		};
		const duckdb::idx_t task_count = std::max<duckdb::idx_t>(1, std::min<duckdb::idx_t>(worker_count, capacity / 4096));
		std::vector<CacheLinePadded<SliceMax>> slices(task_count);
		RunTasks(task_count, [&](duckdb::idx_t task) {
			const size_t begin = capacity * task / task_count, end = capacity * (task + 1) / task_count;
			auto &slice = slices[task].value;
			for (size_t key = begin; key < end; key++) {
				bool has = false;
				q15::Acc sum {};
				for (const auto &worker : workers) {
					const auto &store = worker.value.Store();
					if (store.Has(static_cast<int64_t>(key))) {
						q15::AddAcc(sum, store.Revenue(static_cast<int64_t>(key)));
						has = true;
					}
				}
				if (!has) {
					continue;
				}
				if (!slice.has || sum > slice.revenue) {
					slice.has = true;
					slice.revenue = sum;
					slice.keys.assign(1, static_cast<int64_t>(key));
				} else if (sum == slice.revenue) {
					slice.keys.push_back(static_cast<int64_t>(key));
				}
			}
		});

		const SliceMax *best = nullptr;
		for (const auto &slice : slices) {
			if (slice.value.has && (!best || slice.value.revenue > best->revenue)) {
				best = &slice.value;
			}
		}

		output_rows.clear();
		if (best) {
			std::vector<int64_t> winners;
			for (const auto &slice : slices) {
				if (slice.value.has && slice.value.revenue == best->revenue) {
					winners.insert(winners.end(), slice.value.keys.begin(), slice.value.keys.end());
				}
			}
			std::sort(winners.begin(), winners.end());
			FetchWinningSuppliers(context, *bound_inputs[1], winners, q15::ToHugeint(best->revenue), output_rows);
			std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
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
