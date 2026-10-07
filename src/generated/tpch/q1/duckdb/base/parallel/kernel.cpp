//! Q1 parallel kernel: RunParallelChunkScan over the unfiltered lineitem scan, per-worker q1::Aggregator
//! (SIMD shipdate predicate + aggregation), deterministic ordered merge.

#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_kernel.hpp"
#include "tpch/q1/duckdb/base/q1_agg.hpp"
#include "tpch/q1/duckdb/base/query.hpp"

#include <memory>
#include <vector>

#include "duckdb/common/exception.hpp"

namespace accelerator_codegen {
namespace {

struct WorkerState {
	q1::Aggregator aggregator;
};

class Q1ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q1ParallelKernel(Q1Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 1 || !bound_inputs[0]) {
			throw duckdb::InternalException("Q1 parallel expects one bound lineitem scan");
		}
		const auto worker_count = ParallelWorkerCount(context);
		std::vector<CacheLinePadded<WorkerState>> workers(worker_count);

		// Unfiltered chunk scan: the aggregator applies l_shipdate <= cutoff with SIMD (~98% of rows qualify,
		// so DuckDB's filtered scan only adds selection/slicing overhead).
		RunParallelChunkScan(context, *bound_inputs[0], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     const auto n = chunk.size();
			                     if (n == 0) {
				                     return;
			                     }
			                     q1::Columns cols {&chunk.data[Q1LineitemChunkLayout::QUANTITY_INDEX],
			                                       &chunk.data[Q1LineitemChunkLayout::EXTENDEDPRICE_INDEX],
			                                       &chunk.data[Q1LineitemChunkLayout::DISCOUNT_INDEX],
			                                       &chunk.data[Q1LineitemChunkLayout::TAX_INDEX],
			                                       &chunk.data[Q1LineitemChunkLayout::RETURNFLAG_INDEX],
			                                       &chunk.data[Q1LineitemChunkLayout::LINESTATUS_INDEX],
			                                       &chunk.data[Q1LineitemChunkLayout::SHIPDATE_INDEX],
			                                       params.cutoff_date.days};
			                     workers[worker_id].value.aggregator.Consume(cols, n, nullptr, n);
		                     });

		// Merge in worker order; sums are exact integers, so the result is independent of scheduling.
		q1::GroupMap merged;
		for (const auto &worker : workers) {
			worker.value.aggregator.MergeInto(merged);
		}
		rows = q1::BuildRows(merged);
		parallel_complete = true;
	}

	void Finalize() override {
		if (!parallel_complete) {
			throw duckdb::InternalException("Q1ParallelKernel::Finalize called before RunParallel");
		}
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized) {
			return false;
		}
		return q1::WriteRows(rows, output_offset, output);
	}

private:
	Q1Parameters params;
	std::vector<q1::OutputRow> rows;
	duckdb::idx_t output_offset = 0;
	bool parallel_complete = false;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ1ParallelKernel(const Q1Parameters &params) {
	return duckdb::make_uniq<Q1ParallelKernel>(params);
}

} // namespace accelerator_codegen
