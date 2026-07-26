#include "accelerator_codegen/tpch_q14_aux.hpp"

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/tpch_q14_finalize.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace accelerator_codegen {
namespace {

class Q14AuxFastKernel final : public KernelInstance {
public:
	Q14AuxFastKernel(Q14MonthTotals totals_p) : totals(std::move(totals_p)) {
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
		const auto result =
		    FinalizeQ14PromoRevenue(totals.saw_contribution, totals.total_revenue_raw, totals.promo_revenue_raw);
		if (result.is_null) {
			output.SetValue(0, 0, duckdb::Value(duckdb::LogicalType::DOUBLE));
		} else {
			output.SetValue(0, 0, duckdb::Value::DOUBLE(result.value));
		}
		output.SetCardinality(1);
		emitted = true;
		return true;
	}

private:
	Q14MonthTotals totals;
	bool finalized = false;
	bool emitted = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ14AuxFastKernel(const Q14MonthTotals &totals) {
	return duckdb::make_uniq<Q14AuxFastKernel>(totals);
}

} // namespace accelerator_codegen
