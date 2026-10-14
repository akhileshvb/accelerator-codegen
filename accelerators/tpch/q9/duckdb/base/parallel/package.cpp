#include "backends/duckdb/api/query_runtime.hpp"
#include "tpch/q9/duckdb/base/query.hpp"

#include <memory>
#include <utility>

#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kProfitDecimalWidth = 38;
constexpr uint8_t kProfitDecimalScale = 4;

Q9Parameters BindQ9Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q9_parallel expects exactly 1 argument");
	}
	if (inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q9_parallel color argument must not be NULL");
	}

	Q9Parameters parameters;
	parameters.color = inputs[0].GetValue<std::string>();
	return parameters;
}

QueryBindResult BindQ9ParallelPackage(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ9Parameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q9Parameters>(parameters);
	result.input_scans.push_back(CreateQ9PartScanSpec(parameters));
	result.input_scans.push_back(CreateQ9SupplierScanSpec(parameters));
	result.input_scans.push_back(CreateQ9NationScanSpec(parameters));
	result.input_scans.push_back(CreateQ9OrdersScanSpec(parameters));
	result.input_scans.push_back(CreateQ9PartsuppScanSpec(parameters));
	result.input_scans.push_back(CreateQ9LineitemScanSpec(parameters));
	result.output_schema.names = {"nation", "o_year", "sum_profit"};
	result.output_schema.types = {
	    duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::BIGINT,
	    duckdb::LogicalType::DECIMAL(kProfitDecimalWidth, kProfitDecimalScale),
	};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ9ParallelKernelInstance(const QueryParameters &parameters,
                                                                  const QueryKernelFactoryData *) {
	auto &q9_parameters = dynamic_cast<const Q9Parameters &>(parameters);
	return CreateQ9ParallelKernel(q9_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ9ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q9_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ9ParallelPackage;
	package.create_kernel = CreateQ9ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
