#include "tpch/q22/duckdb/base/query.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "tpch/q22/duckdb/base/q22_common.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;

class Q22Kernel final : public KernelInstance {
public:
	explicit Q22Kernel(Q22Parameters params_p) : params(std::move(params_p)), setup(params) {
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q22InputId>(input_id)) {
		case Q22InputId::ORDERS:
			has_orders.ConsumeChunk(input);
			return;
		case Q22InputId::CUSTOMER:
			q22::ConsumeCustomerChunk(input, setup, matched);
			return;
		default:
			throw duckdb::InternalException("Q22 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		output_rows = q22::BuildOutput(setup, matched, has_orders);
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
			output.SetValue(0, row, duckdb::Value(entry.country_code));
			output.SetValue(1, row, duckdb::Value::BIGINT(entry.customer_count));
			output.SetValue(2, row, duckdb::Value::DECIMAL(entry.total_balance, 38, 2));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	Q22Parameters params;
	q22::CountrySetup setup;
	q22::HasOrders has_orders;
	q22::MatchedCustomers matched;
	std::vector<q22::OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ22Kernel(const Q22Parameters &params) {
	return duckdb::make_uniq<Q22Kernel>(params);
}

} // namespace accelerator_codegen
