#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q17/duckdb/base/query.hpp"

#include <memory>

#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

Q17Parameters BindQ17Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 2) {
		throw duckdb::BinderException("accel_tpch_q17 expects exactly 2 arguments: brand, container");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q17 brand and container must be non-NULL");
	}
	return MakeQ17Parameters(inputs[0].GetValue<std::string>(), inputs[1].GetValue<std::string>());
}

QueryBindResult BindQ17Package(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ17Parameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q17Parameters>(parameters);
	result.input_scans.push_back(CreateQ17LineitemAggScanSpec(parameters));
	result.input_scans.push_back(CreateQ17PartScanSpec(parameters));
	result.input_scans.push_back(CreateQ17LineitemProbeScanSpec(parameters));
	result.output_schema.names = {"avg_yearly"};
	result.output_schema.types = {duckdb::LogicalType::DOUBLE};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ17KernelInstance(const QueryParameters &parameters,
                                                           const QueryKernelFactoryData *) {
	auto &q17_parameters = dynamic_cast<const Q17Parameters &>(parameters);
	return CreateQ17Kernel(q17_parameters);
}

} // namespace

Q17Parameters MakeQ17Parameters(const std::string &brand, const std::string &container) {
	Q17Parameters parameters;
	parameters.brand = brand;
	parameters.container = container;
	return parameters;
}

NativeScanSpec CreateQ17LineitemAggScanSpec(const Q17Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_quantity", "quantity", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	};
	return scan_spec;
}

NativeScanSpec CreateQ17PartScanSpec(const Q17Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "part";
	scan_spec.projected_columns = {
	    {"p_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"p_brand", "brand", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"p_container", "container", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

NativeScanSpec CreateQ17LineitemProbeScanSpec(const Q17Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_quantity", "quantity", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateTpchQ17FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q17";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ17Package;
	package.create_kernel = CreateQ17KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
