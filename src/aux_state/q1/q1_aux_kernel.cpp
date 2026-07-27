#include "accelerator_codegen/tpch_q1_aux.hpp"

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/tpch_q1_finalize.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/value.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kDecimalScale2 = 2;
constexpr uint8_t kDecimalScale4 = 4;
constexpr uint8_t kDecimalScale6 = 6;
constexpr uint8_t kDecimalWidth38 = 38;
constexpr duckdb::idx_t kQ1OutputBatchSize = 8;

class Q1AuxFastKernel final : public KernelInstance {
public:
	explicit Q1AuxFastKernel(std::vector<Q1OutputRow> rows_p) : rows(std::move(rows_p)) {
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
		const auto count = std::min<duckdb::idx_t>(remaining, kQ1OutputBatchSize);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto &result = rows[output_offset + row];
			output.SetValue(0, row, duckdb::Value(UnpackQ1Returnflag(result.group_key)));
			output.SetValue(1, row, duckdb::Value(UnpackQ1Linestatus(result.group_key)));
			output.SetValue(2, row, duckdb::Value::DECIMAL(result.sum_qty, kDecimalWidth38, kDecimalScale2));
			output.SetValue(3, row, duckdb::Value::DECIMAL(result.sum_base_price, kDecimalWidth38, kDecimalScale2));
			output.SetValue(4, row, duckdb::Value::DECIMAL(result.sum_disc_price, kDecimalWidth38, kDecimalScale4));
			output.SetValue(5, row, duckdb::Value::DECIMAL(result.sum_charge, kDecimalWidth38, kDecimalScale6));
			output.SetValue(6, row, duckdb::Value::DOUBLE(result.avg_qty));
			output.SetValue(7, row, duckdb::Value::DOUBLE(result.avg_price));
			output.SetValue(8, row, duckdb::Value::DOUBLE(result.avg_disc));
			output.SetValue(9, row, duckdb::Value::BIGINT(result.count_order));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	std::vector<Q1OutputRow> rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ1AuxFastKernel(std::vector<Q1OutputRow> rows) {
	return duckdb::make_uniq<Q1AuxFastKernel>(std::move(rows));
}

} // namespace accelerator_codegen
