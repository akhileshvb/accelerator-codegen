#include "tpch/q21/duckdb/base/query.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "tpch/q21/duckdb/base/q21_common.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;

class Q21Kernel final : public KernelInstance {
public:
	explicit Q21Kernel(Q21Parameters params_p) : params(std::move(params_p)) {
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q21InputId>(input_id)) {
		case Q21InputId::SUPPLIER:
			if (params.target_nationkey >= 0) {
				q21::CollectSuppliers(input, suppliers);
			}
			return;
		case Q21InputId::ORDERS:
			q21::CollectOrderKeys(input, order_keys);
			return;
		case Q21InputId::LINEITEM:
			if (params.target_nationkey < 0) {
				return;
			}
			PrepareLineitem();
			if (accumulator) {
				q21::ConsumeLineitemChunk<false>(input, finished, *accumulator, sel);
			}
			return;
		default:
			throw duckdb::InternalException("Q21 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		suppliers.Freeze();
		std::vector<int64_t> wait(suppliers.Count(), 0);
		if (accumulator) {
			accumulator->Flush();
			q21::ReduceStates(states.data(), 0, states.size(), suppliers, wait);
		}
		output_rows = q21::BuildOutput(suppliers, wait);
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
			output.SetValue(0, row, duckdb::Value(entry.name));
			output.SetValue(1, row, duckdb::Value::BIGINT(entry.numwait));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	void PrepareLineitem() {
		if (prepared) {
			return;
		}
		prepared = true;
		finished.Build(order_keys);
		order_keys = std::vector<int64_t>();
		if (finished.Size() == 0) {
			return;
		}
		states.Allocate(finished.Size());
		accumulator = std::make_unique<q21::RunAccumulator<false>>(finished, states.data());
	}

	Q21Parameters params;
	q21::SupplierIndex suppliers;
	std::vector<int64_t> order_keys;
	q21::FinishedOrders finished;
	q21::OrderStates states;
	std::unique_ptr<q21::RunAccumulator<false>> accumulator;
	std::vector<uint32_t> sel;
	bool prepared = false;
	std::vector<q21::OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ21Kernel(const Q21Parameters &params) {
	return duckdb::make_uniq<Q21Kernel>(params);
}

} // namespace accelerator_codegen
