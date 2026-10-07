#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q17/duckdb/base/query.hpp"

#include <memory>

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

Q17Parameters BindQ17ParallelParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 2) {
		throw duckdb::BinderException("accel_tpch_q17_parallel expects exactly 2 arguments: brand, container");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q17_parallel brand and container must be non-NULL");
	}
	return MakeQ17Parameters(inputs[0].GetValue<std::string>(), inputs[1].GetValue<std::string>());
}

QueryBindResult BindQ17ParallelPackage(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ17ParallelParameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q17Parameters>(parameters);
	result.input_scans.push_back(CreateQ17PartScanSpec(parameters));
	result.input_scans.push_back(CreateQ17LineitemScanSpec(parameters));
	result.output_schema.names = {"avg_yearly"};
	result.output_schema.types = {duckdb::LogicalType::DOUBLE};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ17ParallelKernelInstance(const QueryParameters &parameters,
                                                                   const QueryKernelFactoryData *) {
	auto &q17_parameters = dynamic_cast<const Q17Parameters &>(parameters);
	return CreateQ17ParallelKernel(q17_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ17ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q17_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ17ParallelPackage;
	package.create_kernel = CreateQ17ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
