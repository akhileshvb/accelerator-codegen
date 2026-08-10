#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q16/base/query.hpp"

#include <memory>

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

Q16Parameters BindQ16ParallelParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 10) {
		throw duckdb::BinderException(
		    "accel_tpch_q16_parallel expects exactly 10 arguments: brand, type_pattern, size1..size8");
	}
	for (const auto &input : inputs) {
		if (input.IsNull()) {
			throw duckdb::BinderException("accel_tpch_q16_parallel arguments must be non-NULL");
		}
	}
	std::array<int32_t, 8> sizes;
	for (duckdb::idx_t index = 0; index < 8; index++) {
		sizes[index] = inputs[2 + index].GetValue<int32_t>();
	}
	return MakeQ16Parameters(inputs[0].GetValue<std::string>(), inputs[1].GetValue<std::string>(), sizes);
}

QueryBindResult BindQ16ParallelPackage(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ16ParallelParameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q16Parameters>(parameters);
	result.input_scans.push_back(CreateQ16SupplierScanSpec(parameters));
	result.input_scans.push_back(CreateQ16PartScanSpec(parameters));
	result.input_scans.push_back(CreateQ16PartsuppScanSpec(parameters));
	result.output_schema.names = {"p_brand", "p_type", "p_size", "supplier_cnt"};
	result.output_schema.types = {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR,
	                              duckdb::LogicalType::INTEGER, duckdb::LogicalType::BIGINT};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ16ParallelKernelInstance(const QueryParameters &parameters,
                                                                   const QueryKernelFactoryData *) {
	auto &q16_parameters = dynamic_cast<const Q16Parameters &>(parameters);
	return CreateQ16ParallelKernel(q16_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ16ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q16_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::INTEGER,
	      duckdb::LogicalType::INTEGER, duckdb::LogicalType::INTEGER, duckdb::LogicalType::INTEGER,
	      duckdb::LogicalType::INTEGER, duckdb::LogicalType::INTEGER, duckdb::LogicalType::INTEGER,
	      duckdb::LogicalType::INTEGER}},
	};
	package.bind_query = BindQ16ParallelPackage;
	package.create_kernel = CreateQ16ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
