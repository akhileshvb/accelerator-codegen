#include "backends/duckdb/api/duckdb_scan_adapter.hpp"
#include "backends/duckdb/api/parallel/parallel_kernel.hpp"
#include "tpch/q17/duckdb/base/query.hpp"

#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/value.hpp"
#include "tpch/q17/duckdb/base/q17_common.hpp"

namespace accelerator_codegen {
namespace {

class Q17ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q17ParallelKernel(Q17Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 2 || !bound_inputs[0] || !bound_inputs[1]) {
			throw duckdb::InternalException("Q17 parallel expects bound part and lineitem scans");
		}

		// part is tiny (200k rows at SF1): scan it serially, build the qualifying bitmap.
		std::vector<int64_t> keys;
		{
			auto source = CreateNativeChunkSource(context, *bound_inputs[0]);
			while (source->Next()) {
				q17::CollectQualifyingParts(source->Chunk(), params, keys);
			}
		}
		q17::QualSet qualifying;
		qualifying.Build(keys);
		if (qualifying.Empty()) {
			return;
		}

		// single parallel pass over lineitem; each worker buffers the rows of qualifying parts.
		const auto worker_count = ParallelWorkerCount(context);
		std::vector<CacheLinePadded<q17::RowBuffer>> workers(static_cast<size_t>(worker_count));
		RunParallelChunkScan(context, *bound_inputs[1], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     q17::ConsumeLineitemChunk(chunk, qualifying, workers[static_cast<size_t>(worker_id)].value);
		                     });

		// Sums are exact integers (int64 quantity, 128-bit price) and the threshold is a function of the
		// full per-part sums, so the result does not depend on worker/row order.
		std::vector<const q17::RowBuffer *> buffers;
		buffers.reserve(workers.size());
		for (const auto &worker : workers) {
			buffers.push_back(&worker.value);
		}
		has_extendedprice_sum = q17::ReduceBuffers(buffers, extendedprice_sum);
	}

	void Finalize() override {
		if (has_extendedprice_sum) {
			auto decimal_sum = duckdb::Value::DECIMAL(extendedprice_sum, 38, 2);
			result_value = decimal_sum.DefaultCastAs(duckdb::LogicalType::DOUBLE).GetValue<double>() / 7.0;
			has_result = true;
		}
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || emitted) {
			return false;
		}
		output.SetCardinality(1);
		if (!has_result) {
			output.SetValue(0, 0, duckdb::Value(duckdb::LogicalType::DOUBLE));
		} else {
			output.SetValue(0, 0, duckdb::Value::DOUBLE(result_value));
		}
		emitted = true;
		return true;
	}

private:
	Q17Parameters params;
	duckdb::hugeint_t extendedprice_sum = duckdb::Hugeint::Convert(0);
	bool has_extendedprice_sum = false;
	double result_value = 0.0;
	bool has_result = false;
	bool finalized = false;
	bool emitted = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ17ParallelKernel(const Q17Parameters &params) {
	return duckdb::make_uniq<Q17ParallelKernel>(params);
}

} // namespace accelerator_codegen
