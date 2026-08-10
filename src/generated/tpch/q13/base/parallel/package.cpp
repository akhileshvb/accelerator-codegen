#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q13/base/query.hpp"

#include <memory>

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

Q13Parameters BindQ13ParallelParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q13_parallel expects exactly 1 argument: comment_pattern");
	}
	if (inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q13_parallel comment_pattern must be non-NULL");
	}
	return MakeQ13Parameters(inputs[0].GetValue<std::string>());
}

QueryBindResult BindQ13ParallelPackage(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ13ParallelParameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q13Parameters>(parameters);
	result.input_scans.push_back(CreateQ13CustomerScanSpec(parameters));
	result.input_scans.push_back(CreateQ13OrdersScanSpec(parameters));
	result.output_schema.names = {"c_count", "custdist"};
	result.output_schema.types = {duckdb::LogicalType::BIGINT, duckdb::LogicalType::BIGINT};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ13ParallelKernelInstance(const QueryParameters &parameters,
                                                                   const QueryKernelFactoryData *) {
	auto &q13_parameters = dynamic_cast<const Q13Parameters &>(parameters);
	return CreateQ13ParallelKernel(q13_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ13ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q13_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ13ParallelPackage;
	package.create_kernel = CreateQ13ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
