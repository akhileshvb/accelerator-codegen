#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q14/base/query.hpp"

#include <memory>

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

Q14Parameters BindQ14Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q14_parallel expects exactly 1 argument: start_date");
	}
	if (inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q14_parallel start_date must be non-NULL");
	}
	return MakeQ14Parameters(inputs[0].GetValue<duckdb::date_t>());
}

QueryBindResult BindQ14ParallelPackage(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ14Parameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q14Parameters>(parameters);
	result.input_scans.push_back(CreateQ14PartScanSpec(parameters));
	result.input_scans.push_back(CreateQ14LineitemScanSpec(parameters));
	result.output_schema.names = {"promo_revenue"};
	result.output_schema.types = {duckdb::LogicalType::DOUBLE};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ14ParallelKernelInstance(const QueryParameters &parameters,
                                                                   const QueryKernelFactoryData *) {
	auto &q14_parameters = dynamic_cast<const Q14Parameters &>(parameters);
	return CreateQ14ParallelKernel(q14_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ14ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q14_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::DATE}},
	};
	package.bind_query = BindQ14ParallelPackage;
	package.create_kernel = CreateQ14ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
