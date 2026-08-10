#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q15/base/query.hpp"

#include <memory>

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;

Q15Parameters BindQ15ParallelParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q15_parallel expects exactly 1 argument: start_date");
	}
	if (inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q15_parallel start_date must be non-NULL");
	}
	return MakeQ15Parameters(inputs[0].GetValue<duckdb::date_t>());
}

QueryBindResult BindQ15ParallelPackage(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ15ParallelParameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q15Parameters>(parameters);
	result.input_scans.push_back(CreateQ15LineitemScanSpec(parameters));
	result.input_scans.push_back(CreateQ15SupplierScanSpec(parameters));
	result.output_schema.names = {"s_suppkey", "s_name", "s_address", "s_phone", "total_revenue"};
	result.output_schema.types = {duckdb::LogicalType::BIGINT, duckdb::LogicalType::VARCHAR,
	                              duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR,
	                              duckdb::LogicalType::DECIMAL(kRevenueDecimalWidth, kRevenueDecimalScale)};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ15ParallelKernelInstance(const QueryParameters &parameters,
                                                                   const QueryKernelFactoryData *) {
	auto &q15_parameters = dynamic_cast<const Q15Parameters &>(parameters);
	return CreateQ15ParallelKernel(q15_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ15ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q15_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::DATE}},
	};
	package.bind_query = BindQ15ParallelPackage;
	package.create_kernel = CreateQ15ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
