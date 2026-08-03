#include "accelerator_codegen/sec_edgar_q1_aux.hpp"

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/sec_edgar_q1_finalize.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/value.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kSecEdgarQ1OutputBatchSize = 64;

class SecEdgarQ1AuxFastKernel final : public KernelInstance {
public:
	explicit SecEdgarQ1AuxFastKernel(std::vector<SecEdgarQ1OutputRow> rows_p) : rows(std::move(rows_p)) {
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
		const auto count = std::min<duckdb::idx_t>(remaining, kSecEdgarQ1OutputBatchSize);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto &result = rows[output_offset + row];
			output.SetValue(0, row, duckdb::Value(result.stmt));
			if (result.rfile_is_null) {
				output.SetValue(1, row, duckdb::Value());
			} else {
				output.SetValue(1, row, duckdb::Value(result.rfile));
			}
			output.SetValue(2, row, duckdb::Value::BIGINT(result.cnt));
			output.SetValue(3, row, duckdb::Value::BIGINT(result.num_filings));
			output.SetValue(4, row, duckdb::Value::DOUBLE(result.avg_line_num));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	std::vector<SecEdgarQ1OutputRow> rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ1AuxFastKernel(std::vector<SecEdgarQ1OutputRow> rows) {
	return duckdb::make_uniq<SecEdgarQ1AuxFastKernel>(std::move(rows));
}

} // namespace accelerator_codegen
