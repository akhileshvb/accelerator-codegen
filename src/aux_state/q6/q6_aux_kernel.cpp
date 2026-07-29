#include "accelerator_codegen/tpch_q6_aux_kernel.hpp"

#include "accelerator_codegen/kernel.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace accelerator_codegen {
namespace {

class Q6AuxFastKernel final : public KernelInstance {
public:
	explicit Q6AuxFastKernel(Q6AccT revenue_raw_p) : revenue_raw(revenue_raw_p) {
	}

	void ConsumeInput(duckdb::idx_t, duckdb::DataChunk &) override {
	}

	void Finalize() override {
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || emitted) {
			return false;
		}
		output.SetValue(0, 0, duckdb::Value::DECIMAL(Q6RevenueFromAccumulator(revenue_raw), kQ6RevenueDecimalWidth,
		                                               kQ6RevenueDecimalScale));
		output.SetCardinality(1);
		emitted = true;
		return true;
	}

private:
	Q6AccT revenue_raw = static_cast<Q6AccT>(0);
	bool finalized = false;
	bool emitted = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ6AuxFastKernel(Q6AccT revenue_raw) {
	return duckdb::make_uniq<Q6AuxFastKernel>(revenue_raw);
}

} // namespace accelerator_codegen
