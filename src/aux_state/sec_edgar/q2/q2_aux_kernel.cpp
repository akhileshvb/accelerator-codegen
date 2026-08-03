#include "accelerator_codegen/sec_edgar_q2_aux.hpp"

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/sec_edgar_q2_finalize.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/value.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kSecEdgarQ2OutputBatchSize = 64;

class SecEdgarQ2AuxFastKernel final : public KernelInstance {
public:
	explicit SecEdgarQ2AuxFastKernel(std::vector<SecEdgarQ2OutputRow> rows_p) : rows(std::move(rows_p)) {
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
		const auto count = std::min<duckdb::idx_t>(remaining, kSecEdgarQ2OutputBatchSize);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto &result = rows[output_offset + row];
			output.SetValue(0, row, duckdb::Value(result.name));
			output.SetValue(1, row, duckdb::Value(result.tag));
			output.SetValue(2, row, duckdb::Value::DOUBLE(result.value));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	std::vector<SecEdgarQ2OutputRow> rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ2AuxFastKernel(std::vector<SecEdgarQ2OutputRow> rows) {
	return duckdb::make_uniq<SecEdgarQ2AuxFastKernel>(std::move(rows));
}

} // namespace accelerator_codegen
