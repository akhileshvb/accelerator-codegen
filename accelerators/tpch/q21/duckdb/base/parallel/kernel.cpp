#include "backends/duckdb/api/duckdb_scan_adapter.hpp"
#include "backends/duckdb/api/parallel/parallel_kernel.hpp"
#include "backends/duckdb/api/parallel/parallel_runtime.hpp"
#include "tpch/q21/duckdb/base/query.hpp"

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/parallel/task.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "tpch/q21/duckdb/base/q21_common.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;

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

struct LineitemWorker {
	std::unique_ptr<q21::RunAccumulator<true>> accumulator;
	std::vector<uint32_t> sel;
};

class Q21ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q21ParallelKernel(Q21Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 3 || !bound_inputs[0] || !bound_inputs[1] || !bound_inputs[2]) {
			throw duckdb::InternalException("Q21 parallel expects bound supplier, orders, lineitem scans");
		}
		if (params.target_nationkey < 0) {
			return;
		}

		q21::SupplierIndex suppliers;
		{
			auto source = CreateNativeChunkSource(context, *bound_inputs[0]);
			while (source->Next()) {
				q21::CollectSuppliers(source->Chunk(), suppliers);
			}
		}
		suppliers.Freeze();

		const auto worker_count = ParallelWorkerCount(context);
		q21::FinishedOrders finished;
		{
			std::vector<CacheLinePadded<std::vector<int64_t>>> key_workers(worker_count);
			RunParallelChunkScan(context, *bound_inputs[1], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
				                     q21::CollectOrderKeys(chunk, key_workers[worker_id].value);
			                     });
			std::vector<int64_t> keys;
			size_t total = 0;
			for (const auto &w : key_workers) {
				total += w.value.size();
			}
			keys.reserve(total);
			for (auto &w : key_workers) {
				keys.insert(keys.end(), w.value.begin(), w.value.end());
				w.value = std::vector<int64_t>();
			}
			finished.Build(keys);
		}

		std::vector<int64_t> wait(suppliers.Count(), 0);
		if (finished.Size() != 0) {
			q21::OrderStates states;
			states.Allocate(finished.Size());
			std::vector<CacheLinePadded<LineitemWorker>> workers(worker_count);
			for (auto &w : workers) {
				w.value.accumulator = std::make_unique<q21::RunAccumulator<true>>(finished, states.data());
			}
			RunParallelChunkScan(context, *bound_inputs[2], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
				                     auto &w = workers[worker_id].value;
				                     q21::ConsumeLineitemChunk<true>(chunk, finished, *w.accumulator, w.sel);
			                     });
			for (auto &w : workers) {
				w.value.accumulator->Flush();
			}

			// Reduce the per-order summaries in parallel over rank ranges; per-range partial counts
			// are integers, so the sum is independent of scheduling.
			const size_t total = states.size();
			const size_t ranges = std::max<size_t>(1, std::min<size_t>(total / 32768 + 1, worker_count * 4));
			std::vector<std::vector<int64_t>> partial(ranges, std::vector<int64_t>(suppliers.Count(), 0));
			std::atomic<size_t> next {0};
			duckdb::TaskExecutor executor(context);
			for (duckdb::idx_t t = 0; t < worker_count; t++) {
				executor.ScheduleTask(duckdb::make_uniq<LambdaExecutorTask>(executor, [&]() {
					for (;;) {
						const size_t r = next.fetch_add(1);
						if (r >= ranges) {
							break;
						}
						q21::ReduceStates(states.data(), total * r / ranges, total * (r + 1) / ranges, suppliers,
						                  partial[r]);
					}
				}));
			}
			executor.WorkOnTasks();
			if (context.interrupted) {
				throw duckdb::InterruptException();
			}
			for (const auto &p : partial) {
				for (size_t i = 0; i < wait.size(); i++) {
					wait[i] += p[i];
				}
			}
		}
		output_rows = q21::BuildOutput(suppliers, wait);
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
	std::vector<q21::OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ21ParallelKernel(const Q21Parameters &params) {
	return duckdb::make_uniq<Q21ParallelKernel>(params);
}

} // namespace accelerator_codegen
