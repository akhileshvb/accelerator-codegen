#include "backends/duckdb/api/query_runtime.hpp"
#include "tpch/q18/duckdb/base/query.hpp"

#include <memory>

#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

Q18Parameters BindQ18ParallelParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q18_parallel expects exactly 1 argument: quantity_threshold");
	}
	if (inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q18_parallel quantity_threshold must be non-NULL");
	}
	return MakeQ18Parameters(inputs[0].GetValue<int64_t>());
}

QueryBindResult BindQ18ParallelPackage(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ18ParallelParameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q18Parameters>(parameters);
	result.input_scans.push_back(CreateQ18LineitemAggScanSpec(parameters));
	result.input_scans.push_back(CreateQ18OrdersScanSpec(parameters));
	result.input_scans.push_back(CreateQ18CustomerScanSpec(parameters));
	result.output_schema.names = {"c_name", "c_custkey", "o_orderkey", "o_orderdate", "o_totalprice", "sum(l_quantity)"};
	result.output_schema.types = {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT, duckdb::LogicalType::BIGINT,
	                              duckdb::LogicalType::DATE, duckdb::LogicalType::DECIMAL(15, 2),
	                              duckdb::LogicalType::DECIMAL(38, 2)};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ18ParallelKernelInstance(const QueryParameters &parameters,
                                                                 const QueryKernelFactoryData *) {
	auto &q18_parameters = dynamic_cast<const Q18Parameters &>(parameters);
	return CreateQ18ParallelKernel(q18_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ18ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q18_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::BIGINT}},
	};
	package.bind_query = BindQ18ParallelPackage;
	package.create_kernel = CreateQ18ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
