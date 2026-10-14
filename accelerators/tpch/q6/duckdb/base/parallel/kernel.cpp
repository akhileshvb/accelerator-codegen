#include "backends/duckdb/api/parallel/parallel_kernel.hpp"
#include "backends/duckdb/api/query_runtime.hpp"
#include "tpch/q6/duckdb/base/q6_filter.hpp"
#include "tpch/q6/duckdb/base/query.hpp"

#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

struct Q6WorkerAgg {
	q6::Accumulator accumulator;
};

class Q6ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q6ParallelKernel(Q6Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 1 || !bound_inputs[0]) {
			throw duckdb::InternalException("Q6 parallel expects one bound lineitem scan");
		}
		const auto worker_count = ParallelWorkerCount(context);
		std::vector<CacheLinePadded<Q6WorkerAgg>> workers(worker_count);

		RunParallelChunkScan(context, *bound_inputs[0], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     workers[worker_id].value.accumulator.Consume(chunk, params);
		                     });

		// Exact integer sums: the merge order cannot change the result.
		q6::Accumulator merged;
		for (const auto &w : workers) {
			merged.Merge(w.value.accumulator);
		}
		has_value = merged.HasValue();
		value = merged.Value();
	}

	void Finalize() override {
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || emitted) {
			return false;
		}
		if (!has_value) {
			output.SetValue(0, 0, duckdb::Value(duckdb::LogicalType::DECIMAL(38, 4)));
		} else {
			output.SetValue(0, 0, duckdb::Value::DECIMAL(value, 38, 4));
		}
		output.SetCardinality(1);
		emitted = true;
		return true;
	}

private:
	Q6Parameters params;
	duckdb::hugeint_t value = duckdb::Hugeint::Convert(0);
	bool has_value = false;
	bool finalized = false;
	bool emitted = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ6ParallelKernel(const Q6Parameters &params) {
	return duckdb::make_uniq<Q6ParallelKernel>(params);
}

} // namespace accelerator_codegen
