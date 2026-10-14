#include "backends/duckdb/api/query_runtime.hpp"
#include "tpch/q12/duckdb/base/query.hpp"

#include <memory>

#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

Q12Parameters BindQ12Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 3) {
		throw duckdb::BinderException(
		    "accel_tpch_q12_parallel expects exactly 3 arguments: shipmode1, shipmode2, start_date");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull() || inputs[2].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q12_parallel arguments must be non-NULL");
	}
	return MakeQ12Parameters(inputs[0].GetValue<std::string>(), inputs[1].GetValue<std::string>(),
	                         inputs[2].GetValue<duckdb::date_t>());
}

QueryBindResult BindQ12ParallelPackage(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ12Parameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q12Parameters>(parameters);
	result.input_scans.push_back(CreateQ12OrdersScanSpec(parameters));
	result.input_scans.push_back(CreateQ12LineitemScanSpec(parameters));
	result.output_schema.names = {"l_shipmode", "high_line_count", "low_line_count"};
	result.output_schema.types = {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::HUGEINT,
	                              duckdb::LogicalType::HUGEINT};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ12ParallelKernelInstance(const QueryParameters &parameters,
                                                                   const QueryKernelFactoryData *) {
	auto &q12_parameters = dynamic_cast<const Q12Parameters &>(parameters);
	return CreateQ12ParallelKernel(q12_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ12ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q12_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::DATE}},
	};
	package.bind_query = BindQ12ParallelPackage;
	package.create_kernel = CreateQ12ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
