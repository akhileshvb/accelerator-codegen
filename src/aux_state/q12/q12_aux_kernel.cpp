#include "accelerator_codegen/tpch_q12_aux_kernel.hpp"

#include "accelerator_codegen/kernel.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace accelerator_codegen {
namespace {

class Q12AuxFastKernel final : public KernelInstance {
public:
	explicit Q12AuxFastKernel(std::vector<Q12OutputRow> rows_p) : rows(std::move(rows_p)) {
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
		const auto count = std::min<duckdb::idx_t>(remaining, static_cast<duckdb::idx_t>(2048));
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto &result = rows[output_offset + row];
			output.SetValue(0, row, duckdb::Value(result.shipmode));
			output.SetValue(1, row, duckdb::Value::HUGEINT(duckdb::hugeint_t(result.high_line_count)));
			output.SetValue(2, row, duckdb::Value::HUGEINT(duckdb::hugeint_t(result.low_line_count)));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	std::vector<Q12OutputRow> rows;
	bool finalized = false;
	duckdb::idx_t output_offset = 0;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ12AuxFastKernel(std::vector<Q12OutputRow> rows) {
	return duckdb::make_uniq<Q12AuxFastKernel>(std::move(rows));
}

} // namespace accelerator_codegen
