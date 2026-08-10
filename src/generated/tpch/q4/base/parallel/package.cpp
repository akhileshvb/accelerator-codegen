#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q4/base/query.hpp"

#include <memory>

#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

Q4Parameters BindQ4Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q4_parallel expects exactly 1 argument");
	}
	return MakeQ4Parameters(inputs[0].GetValue<duckdb::date_t>());
}

QueryBindResult BindQ4ParallelPackage(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ4Parameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q4Parameters>(parameters);
	result.input_scans.push_back(CreateQ4LineitemScanSpec(parameters));
	result.input_scans.push_back(CreateQ4OrdersScanSpec(parameters));
	result.output_schema.names = {"o_orderpriority", "order_count"};
	result.output_schema.types = {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ4ParallelKernelInstance(const QueryParameters &parameters,
                                                                  const QueryKernelFactoryData *) {
	auto &q4_parameters = dynamic_cast<const Q4Parameters &>(parameters);
	return CreateQ4ParallelKernel(q4_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ4ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q4_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::DATE}},
	};
	package.bind_query = BindQ4ParallelPackage;
	package.create_kernel = CreateQ4ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
