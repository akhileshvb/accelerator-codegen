#include "accelerator_codegen/sec_edgar_q24_aux.hpp"

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/sec_edgar_q24_finalize.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/value.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kSecEdgarQ24OutputBatchSize = 64;

class SecEdgarQ24AuxFastKernel final : public KernelInstance {
public:
	explicit SecEdgarQ24AuxFastKernel(std::shared_ptr<const SecEdgarQ24OutputPartition> output_p)
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
		const auto count = std::min<duckdb::idx_t>(remaining, kSecEdgarQ24OutputBatchSize);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto &result = rows[output_offset + row];
			output_chunk.SetValue(0, row, duckdb::Value(result.tag));
			output_chunk.SetValue(1, row, duckdb::Value(result.version));
			output_chunk.SetValue(2, row, duckdb::Value::BIGINT(result.cnt));
			output_chunk.SetValue(3, row, duckdb::Value::DOUBLE(result.total));
		}
		output_chunk.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	std::shared_ptr<const SecEdgarQ24OutputPartition> output;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance>
CreateSecEdgarQ24AuxFastKernel(std::shared_ptr<const SecEdgarQ24OutputPartition> output) {
	return duckdb::make_uniq<SecEdgarQ24AuxFastKernel>(std::move(output));
}

} // namespace accelerator_codegen
