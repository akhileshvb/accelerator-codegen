#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q5/base/query.hpp"

#include <memory>

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;

Q5Parameters BindQ5ParallelParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 2) {
		throw duckdb::BinderException("accel_tpch_q5_parallel expects exactly 2 arguments: region_name, start_date");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q5_parallel arguments must be non-NULL");
	}
	return MakeQ5Parameters(inputs[0].GetValue<std::string>(), inputs[1].GetValue<duckdb::date_t>());
}

QueryBindResult BindQ5ParallelPackage(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ5ParallelParameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q5Parameters>(parameters);
	result.input_scans.push_back(CreateQ5RegionScanSpec(parameters));
	result.input_scans.push_back(CreateQ5NationScanSpec(parameters));
	result.input_scans.push_back(CreateQ5CustomerScanSpec(parameters));
	result.input_scans.push_back(CreateQ5SupplierScanSpec(parameters));
	result.input_scans.push_back(CreateQ5OrdersScanSpec(parameters));
	result.input_scans.push_back(CreateQ5LineitemScanSpec(parameters));
	result.output_schema.names = {"n_name", "revenue"};
	result.output_schema.types = {duckdb::LogicalType::VARCHAR,
	                              duckdb::LogicalType::DECIMAL(kRevenueDecimalWidth, kRevenueDecimalScale)};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ5ParallelKernelInstance(const QueryParameters &parameters,
                                                                   const QueryKernelFactoryData *) {
	auto &q5_parameters = dynamic_cast<const Q5Parameters &>(parameters);
	return CreateQ5ParallelKernel(q5_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ5ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q5_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::DATE}},
	};
	package.bind_query = BindQ5ParallelPackage;
	package.create_kernel = CreateQ5ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
