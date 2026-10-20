#include "backends/duckdb/api/duckdb_scan_adapter.hpp"
#include "backends/duckdb/api/parallel/parallel_kernel.hpp"
#include "backends/duckdb/api/parallel/parallel_runtime.hpp"
#include "tpch/q10/duckdb/base/query.hpp"
#include "tpch/q10/duckdb/base/q10_common.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;
constexpr duckdb::idx_t kOutputBatchSize = 64;

using q10::OrderPair;

struct Q10OutputRow {
	int64_t custkey = 0;
	std::string name;
	duckdb::hugeint_t revenue;
	duckdb::hugeint_t acctbal;
	std::string nation_name;
	std::string address;
	std::string phone;
	std::string comment;
};

void BuildNationNames(duckdb::ClientContext &context, const duckdb::FunctionData &nation_bind,
                      std::array<std::string, q10::kMaxNationSlots> &names) {
	auto source = CreateNativeChunkSource(context, nation_bind);
	while (source->Next()) {
		q10::ConsumeNationChunk(source->Chunk(), names);
	}
}

class Q10ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q10ParallelKernel(Q10Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 4 || !bound_inputs[0] || !bound_inputs[1] || !bound_inputs[2] ||
		    !bound_inputs[3]) {
			throw duckdb::InternalException("Q10 parallel expects bound nation, orders, lineitem, customer scans");
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
				std::cerr << "q10_parallel_profile " << label << "_ns="
				          << std::chrono::duration_cast<std::chrono::nanoseconds>(now - lap).count() << "\n";
				lap = now;
			}
		};
		const auto worker_count = ParallelWorkerCount(context);

		std::array<std::string, q10::kMaxNationSlots> nation_names;
		BuildNationNames(context, *bound_inputs[0], nation_names);
		tick("nation");

		// Orders (date-filtered by the scan): collect (orderkey, custkey) pairs per worker.
		std::vector<CacheLinePadded<std::vector<OrderPair>>> order_parts(static_cast<size_t>(worker_count));
		RunParallelChunkScan(context, *bound_inputs[1], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     q10::CollectOrdersChunk(chunk, order_parts[static_cast<size_t>(worker_id)].value);
		                     });
		tick("orders_scan");
		q10::OrderCustomerIndex index;
		{
			std::vector<std::vector<OrderPair>> parts;
			parts.reserve(order_parts.size());
			for (auto &part : order_parts) {
				parts.push_back(std::move(part.value));
			}
			index.Build(parts);
		}
		tick("index_build");

		if (!index.Empty()) {
			struct LineitemWorker {
				q10::Accumulator acc;
			};
			std::vector<CacheLinePadded<LineitemWorker>> workers(static_cast<size_t>(worker_count));
			for (auto &worker : workers) {
				worker.value.acc.Init(index.CustomerCount());
			}
			RunParallelChunkScan(context, *bound_inputs[2], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
				                     q10::AccumulateLineitemChunk(chunk, index, workers[static_cast<size_t>(worker_id)].value.acc);
			                     });
			tick("lineitem");
			for (size_t worker = 1; worker < workers.size(); worker++) {
				workers[0].value.acc.MergeFrom(workers[worker].value.acc);
			}
			tick("merge");

			q10::CustomerScanInput scan_input;
			scan_input.index = &index;
			scan_input.acc = &workers[0].value.acc;
			scan_input.nation_names = &nation_names;
			std::vector<CacheLinePadded<q10::TopK>> heaps;
			const auto run_customer_scan = [&](const q10::Floor &floor) {
				heaps.clear();
				heaps.resize(static_cast<size_t>(worker_count));
				for (auto &heap : heaps) {
					heap.value.SetFloor(floor);
				}
				RunParallelFilteredScan(
				    context, *bound_inputs[3], worker_count,
				    [&](duckdb::idx_t worker_id, duckdb::TransactionData transaction,
				        duckdb::CollectionScanState &scan_state, duckdb::RowGroup &row_group,
				        duckdb::SelectionVector &sel, duckdb::idx_t count) {
					    q10::ProcessCustomerVector(transaction, scan_state, row_group, sel, count, scan_input,
					                               heaps[static_cast<size_t>(worker_id)].value);
				    });
			};
			const auto floor = q10::ComputeFloor(index, workers[0].value.acc);
			run_customer_scan(floor);
			if (floor.active) {
				size_t found = 0;
				for (auto &heap : heaps) {
					found += heap.value.Rows().size();
				}
				if (found < q10::kResultLimit) {
					// Some of the top-ranked customers have no customer row / nation: rank without the floor.
					run_customer_scan(q10::Floor {});
				}
			}
			tick("customer");
			std::vector<q10::TopK *> heap_ptrs;
			for (auto &heap : heaps) {
				heap_ptrs.push_back(&heap.value);
			}
			auto rows = q10::FinishTopK(std::move(heap_ptrs));
			output_rows.clear();
			output_rows.reserve(rows.size());
			for (auto &row : rows) {
				Q10OutputRow out;
				out.custkey = row.custkey;
				out.name = std::move(row.name);
				out.revenue = q10::ToHugeint(row.revenue);
				out.acctbal = duckdb::hugeint_t(row.acctbal_raw);
				out.nation_name = nation_names[static_cast<size_t>(row.nationkey)];
				out.address = std::move(row.address);
				out.phone = std::move(row.phone);
				out.comment = std::move(row.comment);
				output_rows.push_back(std::move(out));
			}
			tick("output");
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
			output.SetValue(0, row, duckdb::Value::BIGINT(entry.custkey));
			output.SetValue(1, row, duckdb::Value(entry.name));
			output.SetValue(2, row, duckdb::Value::DECIMAL(entry.revenue, kRevenueDecimalWidth, kRevenueDecimalScale));
			output.SetValue(3, row, duckdb::Value::DECIMAL(entry.acctbal, 15, 2));
			output.SetValue(4, row, duckdb::Value(entry.nation_name));
			output.SetValue(5, row, duckdb::Value(entry.address));
			output.SetValue(6, row, duckdb::Value(entry.phone));
			output.SetValue(7, row, duckdb::Value(entry.comment));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	Q10Parameters params;
	std::vector<Q10OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ10ParallelKernel(const Q10Parameters &params) {
	return duckdb::make_uniq<Q10ParallelKernel>(params);
}

} // namespace accelerator_codegen
