#include "backends/duckdb/engine/duckdb_scan_adapter.hpp"
#include "backends/duckdb/api/parallel/parallel_kernel.hpp"
#include "backends/duckdb/api/parallel/parallel_runtime.hpp"
#include "tpch/q15/duckdb/base/q15_revenue.hpp"
#include "tpch/q15/duckdb/base/query.hpp"

#include <algorithm>
#include <string>
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
		const auto worker_count = ParallelWorkerCount(context);
		std::vector<CacheLinePadded<q15::ChunkAggregator>> workers(worker_count);

		RunParallelChunkScan(context, *bound_inputs[0], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     workers[worker_id].value.Consume(chunk, params);
		                     });

		// Serial merge of the (small, dense) per-worker stores: integer sums, so order cannot matter.
		q15::RevenueStore merged;
		for (const auto &worker : workers) {
			merged.MergeFrom(worker.value.Store());
		}

		duckdb::hugeint_t max_revenue;
		bool has_max_revenue = false;
		for (const auto suppkey : merged.Active()) {
			const auto revenue = q15::ToHugeint(merged.Revenue(suppkey));
			if (!has_max_revenue || duckdb::Hugeint::GreaterThan(revenue, max_revenue)) {
				max_revenue = revenue;
				has_max_revenue = true;
			}
		}

		output_rows.clear();
		if (has_max_revenue) {
			std::unordered_set<int64_t> winners;
			for (const auto suppkey : merged.Active()) {
				if (duckdb::Hugeint::Equals(q15::ToHugeint(merged.Revenue(suppkey)), max_revenue)) {
					winners.insert(suppkey);
				}
			}
			FetchWinningSuppliers(context, *bound_inputs[1], winners, max_revenue, output_rows);
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
