#include "backends/duckdb/api/query_runtime.hpp"
#include "tpch/q8/duckdb/base/query.hpp"
#include "tpch/q8/duckdb/base/q8_bind.hpp"

#include <memory>
#include <sstream>

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen {
namespace {

Q8Parameters BindQ8ParallelParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 3) {
		throw duckdb::BinderException(
		    "accel_tpch_q8_parallel expects exactly 3 arguments: nation_name, region_name, part_type");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull() || inputs[2].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q8_parallel arguments must be non-NULL");
	}
	return MakeQ8Parameters(inputs[0].GetValue<std::string>(), inputs[1].GetValue<std::string>(),
	                        inputs[2].GetValue<std::string>());
}

QueryBindResult BindQ8ParallelPackage(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ8ParallelParameters(input.inputs);
	ResolveQ8BindParameters(context, parameters);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q8Parameters>(parameters);
	result.input_scans.push_back(CreateQ8PartScanSpec(parameters));
	result.input_scans.push_back(CreateQ8SupplierScanSpec(parameters));
	result.input_scans.push_back(CreateQ8CustomerScanSpec(parameters));
	result.input_scans.push_back(CreateQ8OrdersScanSpec(parameters));
	result.input_scans.push_back(CreateQ8LineitemScanSpec(parameters));
	result.output_schema.names = {"o_year", "mkt_share"};
	result.output_schema.types = {duckdb::LogicalType::BIGINT, duckdb::LogicalType::DOUBLE};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ8ParallelKernelInstance(const QueryParameters &parameters,
                                                                  const QueryKernelFactoryData *) {
	auto &q8_parameters = dynamic_cast<const Q8Parameters &>(parameters);
	return CreateQ8ParallelKernel(q8_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ8ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q8_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ8ParallelPackage;
	package.create_kernel = CreateQ8ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
