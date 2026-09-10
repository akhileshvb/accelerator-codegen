#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q7/duckdb/base/query.hpp"

#include <memory>

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;

Q7Parameters BindQ7ParallelParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 2) {
		throw duckdb::BinderException("accel_tpch_q7_parallel expects exactly 2 arguments: nation1, nation2");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q7_parallel arguments must be non-NULL");
	}
	return MakeQ7Parameters(inputs[0].GetValue<std::string>(), inputs[1].GetValue<std::string>());
}

QueryBindResult BindQ7ParallelPackage(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ7ParallelParameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q7Parameters>(parameters);
	result.input_scans.push_back(CreateQ7NationScanSpec(parameters));
	result.input_scans.push_back(CreateQ7CustomerScanSpec(parameters));
	result.input_scans.push_back(CreateQ7SupplierScanSpec(parameters));
	result.input_scans.push_back(CreateQ7OrdersScanSpec(parameters));
	result.input_scans.push_back(CreateQ7LineitemScanSpec(parameters));
	result.output_schema.names = {"supp_nation", "cust_nation", "l_year", "revenue"};
	result.output_schema.types = {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT,
	                              duckdb::LogicalType::DECIMAL(kRevenueDecimalWidth, kRevenueDecimalScale)};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ7ParallelKernelInstance(const QueryParameters &parameters,
                                                                   const QueryKernelFactoryData *) {
	auto &q7_parameters = dynamic_cast<const Q7Parameters &>(parameters);
	return CreateQ7ParallelKernel(q7_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ7ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q7_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ7ParallelPackage;
	package.create_kernel = CreateQ7ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
