#include "backends/duckdb/api/query_runtime.hpp"
#include "tpch/q13/duckdb/base/query.hpp"

#include <memory>

#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

Q13Parameters BindQ13Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q13 expects exactly 1 argument: comment_pattern");
	}
	if (inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q13 comment_pattern must be non-NULL");
	}
	return MakeQ13Parameters(inputs[0].GetValue<std::string>());
}

QueryBindResult BindQ13Package(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ13Parameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q13Parameters>(parameters);
	result.input_scans.push_back(CreateQ13CustomerScanSpec(parameters));
	result.input_scans.push_back(CreateQ13OrdersScanSpec(parameters));
	result.output_schema.names = {"c_count", "custdist"};
	result.output_schema.types = {duckdb::LogicalType::BIGINT, duckdb::LogicalType::BIGINT};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ13KernelInstance(const QueryParameters &parameters,
                                                           const QueryKernelFactoryData *) {
	auto &q13_parameters = dynamic_cast<const Q13Parameters &>(parameters);
	return CreateQ13Kernel(q13_parameters);
}

} // namespace

Q13Parameters MakeQ13Parameters(const std::string &comment_pattern) {
	Q13Parameters parameters;
	parameters.comment_pattern = comment_pattern;
	return parameters;
}

NativeScanSpec CreateQ13CustomerScanSpec(const Q13Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "customer";
	scan_spec.projected_columns = {
	    {"c_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	};
	return scan_spec;
}

NativeScanSpec CreateQ13OrdersScanSpec(const Q13Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "orders";
	scan_spec.projected_columns = {
	    {"o_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_comment", "comment", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateTpchQ13FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q13";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ13Package;
	package.create_kernel = CreateQ13KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
