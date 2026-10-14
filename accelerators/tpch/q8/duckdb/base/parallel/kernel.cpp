#include "backends/duckdb/engine/duckdb_scan_adapter.hpp"
#include "backends/duckdb/api/parallel/parallel_kernel.hpp"
#include "backends/duckdb/api/parallel/parallel_runtime.hpp"
#include "tpch/q8/duckdb/base/query.hpp"
#include "tpch/q8/duckdb/base/q8_common.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <thread>
#include <iostream>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;

struct Q8OutputRow {
	int64_t year = 0;
	double mkt_share = 0.0;
};

class Q8ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q8ParallelKernel(Q8Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 5 || !bound_inputs[0] || !bound_inputs[1] || !bound_inputs[2] ||
		    !bound_inputs[3] || !bound_inputs[4]) {
			throw duckdb::InternalException("Q8 parallel expects bound part, supplier, customer, orders, lineitem scans");
		}
		using clock = std::chrono::steady_clock;
		const bool profile = [] {
			const char *env = std::getenv("ACCEL_PARALLEL_PROFILE");
			return env != nullptr && env[0] == '1';
		}();
		auto lap = clock::now();
		const auto tick = [&](const char *label) {
			if (profile) {
				const auto now = clock::now();
				std::cerr << "q8_parallel_profile " << label << "_ns="
				          << std::chrono::duration_cast<std::chrono::nanoseconds>(now - lap).count() << "\n";
				lap = now;
			}
		};

		output_rows.clear();
		if (!q8::HasRegionNations(params)) {
			return;
		}
		const auto worker_count = ParallelWorkerCount(context);
		q8::Tables tables;

		// The part scan is only needed by the lineitem phase: run it on a helper thread next to the
		// supplier/customer/orders phases (the part and customer tables are 1-2 row groups each, so neither
		// scan could use all workers by itself).
		std::exception_ptr part_error;
		std::thread part_thread([&] {
			try {
				// Per-worker key lists, folded into the bitmap single-threaded afterwards.
				std::vector<CacheLinePadded<std::vector<int64_t>>> part_keys(static_cast<size_t>(worker_count));
				RunParallelChunkScan(context, *bound_inputs[0], worker_count,
				                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
					                     q8::CollectPartKeys(chunk, params.part_type,
					                                         part_keys[static_cast<size_t>(worker_id)].value);
				                     });
				std::vector<std::vector<int64_t>> parts;
				for (auto &keys : part_keys) {
					parts.push_back(std::move(keys.value));
				}
				q8::ApplyKeys(tables.parts, parts);
			} catch (...) {
				part_error = std::current_exception();
			}
		});
		struct JoinGuard {
			std::thread &thread;
			~JoinGuard() {
				if (thread.joinable()) {
					thread.join();
				}
			}
		} join_guard {part_thread};
		{
			auto source = CreateNativeChunkSource(context, *bound_inputs[1]);
			while (source->Next()) {
				q8::ForEachSupplier(source->Chunk(), [&](int64_t suppkey, int32_t nationkey) {
					tables.suppliers.Store(suppkey, nationkey);
				});
			}
		}
		tick("supplier");
		{
			std::vector<CacheLinePadded<std::vector<int64_t>>> customer_keys(static_cast<size_t>(worker_count));
			RunParallelChunkScan(context, *bound_inputs[2], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
				                     q8::CollectCustomerKeys(chunk, params.region_nations,
				                                             customer_keys[static_cast<size_t>(worker_id)].value);
			                     });
			std::vector<std::vector<int64_t>> parts;
			for (auto &keys : customer_keys) {
				parts.push_back(std::move(keys.value));
			}
			q8::ApplyKeys(tables.customers, parts);
		}
		tick("customer");
		{
			std::vector<CacheLinePadded<std::vector<q8::OrderSlot>>> order_slots(static_cast<size_t>(worker_count));
			RunParallelChunkScan(context, *bound_inputs[3], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
				                     q8::CollectOrders(chunk, tables.customers,
				                                       order_slots[static_cast<size_t>(worker_id)].value);
			                     });
			std::vector<std::vector<q8::OrderSlot>> parts;
			for (auto &slots : order_slots) {
				parts.push_back(std::move(slots.value));
			}
			q8::ApplyOrderSlots(tables.orders, parts);
		}
		tick("orders");
		part_thread.join();
		if (part_error) {
			std::rethrow_exception(part_error);
		}
		tick("part_join");

		std::vector<CacheLinePadded<q8::YearVolumes>> workers(static_cast<size_t>(worker_count));
		RunParallelChunkScan(context, *bound_inputs[4], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     q8::ProcessLineitemChunk(tables, params.target_nationkey, chunk,
			                                              workers[static_cast<size_t>(worker_id)].value);
		                     });
		tick("lineitem");

		q8::YearVolumes merged {};
		for (const auto &worker : workers) {
			for (int slot = 0; slot < q8::kYearSlots; slot++) {
				merged[static_cast<size_t>(slot)].nation_volume += worker.value[static_cast<size_t>(slot)].nation_volume;
				merged[static_cast<size_t>(slot)].total_volume += worker.value[static_cast<size_t>(slot)].total_volume;
			}
		}
		for (int slot = 0; slot < q8::kYearSlots; slot++) {
			const auto &state = merged[static_cast<size_t>(slot)];
			if (state.total_volume == 0) {
				continue;
			}
			Q8OutputRow row;
			row.year = q8::kMinYear + slot;
			row.mkt_share = q8::ComputeMarketShare(state);
			output_rows.push_back(row);
		}
		std::sort(output_rows.begin(), output_rows.end(),
		          [](const Q8OutputRow &lhs, const Q8OutputRow &rhs) { return lhs.year < rhs.year; });
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
			output.SetValue(0, row, duckdb::Value::BIGINT(entry.year));
			output.SetValue(1, row, duckdb::Value::DOUBLE(entry.mkt_share));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	Q8Parameters params;
	std::vector<Q8OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ8ParallelKernel(const Q8Parameters &params) {
	return duckdb::make_uniq<Q8ParallelKernel>(params);
}

} // namespace accelerator_codegen
