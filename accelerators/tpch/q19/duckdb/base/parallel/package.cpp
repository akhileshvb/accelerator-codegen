#include "backends/duckdb/api/query_runtime.hpp"
#include "tpch/q19/duckdb/base/query.hpp"

#include <memory>

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

Q19Parameters BindQ19ParallelParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 6) {
		throw duckdb::BinderException(
		    "accel_tpch_q19_parallel expects exactly 6 arguments: brand1, qty_low1, brand2, qty_low2, brand3, qty_low3");
	}
	for (duckdb::idx_t i = 0; i < inputs.size(); i++) {
		if (inputs[i].IsNull()) {
			throw duckdb::BinderException("accel_tpch_q19_parallel arguments must be non-NULL");
		}
	}
	return MakeQ19Parameters(inputs[0].GetValue<std::string>(), inputs[1].GetValue<int64_t>(),
	                         inputs[2].GetValue<std::string>(), inputs[3].GetValue<int64_t>(),
	                         inputs[4].GetValue<std::string>(), inputs[5].GetValue<int64_t>());
}

QueryBindResult BindQ19ParallelPackage(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ19ParallelParameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q19Parameters>(parameters);
	result.input_scans.push_back(CreateQ19PartScanSpec(parameters));
	result.input_scans.push_back(CreateQ19LineitemScanSpec(parameters));
	result.output_schema.names = {"revenue"};
	result.output_schema.types = {duckdb::LogicalType::DECIMAL(38, 4)};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ19ParallelKernelInstance(const QueryParameters &parameters,
                                                                   const QueryKernelFactoryData *) {
	auto &q19_parameters = dynamic_cast<const Q19Parameters &>(parameters);
	return CreateQ19ParallelKernel(q19_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ19ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q19_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT, duckdb::LogicalType::VARCHAR,
	      duckdb::LogicalType::BIGINT, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT}},
	};
	package.bind_query = BindQ19ParallelPackage;
	package.create_kernel = CreateQ19ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
