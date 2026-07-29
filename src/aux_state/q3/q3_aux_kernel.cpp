#include "accelerator_codegen/tpch_q3_aux_kernel.hpp"

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/tpch_q3_finalize.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace accelerator_codegen {
namespace {

class Q3AuxFastKernel final : public KernelInstance {
public:
	explicit Q3AuxFastKernel(std::vector<Q3OutputRow> rows_p) : rows(std::move(rows_p)) {
	}

	void ConsumeInput(duckdb::idx_t, duckdb::DataChunk &) override {
	}

	void Finalize() override {
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || output_offset >= rows.size()) {
			return false;
		}
		const auto remaining = rows.size() - output_offset;
		const auto count = std::min<duckdb::idx_t>(remaining, kQ3TopK);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto &result = rows[output_offset + row];
			output.SetValue(0, row, duckdb::Value::BIGINT(result.orderkey));
			output.SetValue(1, row, duckdb::Value::DECIMAL(result.revenue, kQ3RevenueDecimalWidth, kQ3RevenueDecimalScale));
			output.SetValue(2, row, duckdb::Value::DATE(result.orderdate));
			output.SetValue(3, row, duckdb::Value::INTEGER(result.shippriority));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	std::vector<Q3OutputRow> rows;
	bool finalized = false;
	duckdb::idx_t output_offset = 0;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ3AuxFastKernel(std::vector<Q3OutputRow> rows) {
	return duckdb::make_uniq<Q3AuxFastKernel>(std::move(rows));
}

} // namespace accelerator_codegen
