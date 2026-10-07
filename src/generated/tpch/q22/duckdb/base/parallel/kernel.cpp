#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_kernel.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_runtime.hpp"
#include "tpch/q22/duckdb/base/query.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "tpch/q22/duckdb/base/q22_common.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;

class Q22ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q22ParallelKernel(Q22Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 2 || !bound_inputs[0] || !bound_inputs[1]) {
			throw duckdb::InternalException("Q22 parallel expects orders and customer scans");
		}
		const q22::CountrySetup setup(params);
		const auto worker_count = ParallelWorkerCount(context);

		// customer: one parallel pass; workers append matching customers (exact integer sums).
		std::vector<CacheLinePadded<q22::MatchedCustomers>> customer_workers(static_cast<size_t>(worker_count));
		RunParallelChunkScan(context, *bound_inputs[1], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     q22::ConsumeCustomerChunk(chunk, setup, customer_workers[worker_id].value);
		                     });
		// Merge in a canonical order (sorted by custkey is unnecessary: results are order-independent sums).
		q22::MatchedCustomers matched;
		for (const auto &w : customer_workers) {
			matched.Merge(w.value);
		}

		// orders: per-worker bit vectors, OR-merged.
		std::vector<CacheLinePadded<q22::HasOrders>> order_workers(static_cast<size_t>(worker_count));
		RunParallelChunkScan(context, *bound_inputs[0], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     order_workers[worker_id].value.ConsumeChunk(chunk);
		                     });
		q22::HasOrders has_orders;
		for (const auto &w : order_workers) {
			has_orders.MergeFrom(w.value);
		}

		output_rows = q22::BuildOutput(setup, matched, has_orders);
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
			output.SetValue(0, row, duckdb::Value(entry.country_code));
			output.SetValue(1, row, duckdb::Value::BIGINT(entry.customer_count));
			output.SetValue(2, row, duckdb::Value::DECIMAL(entry.total_balance, 38, 2));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	Q22Parameters params;
	std::vector<q22::OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ22ParallelKernel(const Q22Parameters &params) {
	return duckdb::make_uniq<Q22ParallelKernel>(params);
}

} // namespace accelerator_codegen
