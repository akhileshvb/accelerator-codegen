#include "accelerator_codegen/sec_edgar_q4_aux.hpp"

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/sec_edgar_q4_finalize.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/value.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kSecEdgarQ4OutputBatchSize = 64;

class SecEdgarQ4AuxFastKernel final : public KernelInstance {
public:
	explicit SecEdgarQ4AuxFastKernel(std::shared_ptr<const SecEdgarQ4OutputPartition> output_p)
	    : output(std::move(output_p)) {
	}

	void ConsumeInput(duckdb::idx_t, duckdb::DataChunk &) override {
	}

	void Finalize() override {
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output_chunk) override {
		if (!finalized || !output || output_offset >= output->output_rows.size()) {
			return false;
		}
		const auto &rows = output->output_rows;
		const auto remaining = rows.size() - output_offset;
		const auto count = std::min<duckdb::idx_t>(remaining, kSecEdgarQ4OutputBatchSize);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto &result = rows[output_offset + row];
			output_chunk.SetValue(0, row, duckdb::Value::INTEGER(result.sic));
			output_chunk.SetValue(1, row, duckdb::Value(result.tlabel));
			output_chunk.SetValue(2, row, duckdb::Value(result.stmt));
			output_chunk.SetValue(3, row, duckdb::Value::BIGINT(result.num_companies));
			output_chunk.SetValue(4, row, duckdb::Value::DOUBLE(result.total_value));
			output_chunk.SetValue(5, row, duckdb::Value::DOUBLE(result.avg_value));
		}
		output_chunk.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	std::shared_ptr<const SecEdgarQ4OutputPartition> output;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance>
CreateSecEdgarQ4AuxFastKernel(std::shared_ptr<const SecEdgarQ4OutputPartition> output) {
	return duckdb::make_uniq<SecEdgarQ4AuxFastKernel>(std::move(output));
}

} // namespace accelerator_codegen
