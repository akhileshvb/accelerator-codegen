#include "tpch/q17/duckdb/base/query.hpp"

#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/value.hpp"
#include "tpch/q17/duckdb/base/q17_common.hpp"

namespace accelerator_codegen {
namespace {

class Q17Kernel final : public KernelInstance {
public:
	explicit Q17Kernel(Q17Parameters params_p) : params(std::move(params_p)) {
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q17InputId>(input_id)) {
		case Q17InputId::PART:
			q17::CollectQualifyingParts(input, params, qualifying_keys);
			return;
		case Q17InputId::LINEITEM:
			if (!qualifying_built) {
				qualifying.Build(qualifying_keys);
				qualifying_built = true;
			}
			q17::ConsumeLineitemChunk(input, qualifying, buffer);
			return;
		default:
			throw duckdb::InternalException("Q17 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		duckdb::hugeint_t sum = duckdb::Hugeint::Convert(0);
		if (q17::ReduceBuffers({&buffer}, sum)) {
			auto decimal_sum = duckdb::Value::DECIMAL(sum, 38, 2);
			result_value = decimal_sum.DefaultCastAs(duckdb::LogicalType::DOUBLE).GetValue<double>() / 7.0;
			has_result = true;
		}
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || emitted) {
			return false;
		}
		output.SetCardinality(1);
		if (!has_result) {
			output.SetValue(0, 0, duckdb::Value(duckdb::LogicalType::DOUBLE));
		} else {
			output.SetValue(0, 0, duckdb::Value::DOUBLE(result_value));
		}
		emitted = true;
		return true;
	}

private:
	Q17Parameters params;
	std::vector<int64_t> qualifying_keys;
	q17::QualSet qualifying;
	bool qualifying_built = false;
	q17::RowBuffer buffer;
	double result_value = 0.0;
	bool has_result = false;
	bool finalized = false;
	bool emitted = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ17Kernel(const Q17Parameters &params) {
	return duckdb::make_uniq<Q17Kernel>(params);
}

} // namespace accelerator_codegen
