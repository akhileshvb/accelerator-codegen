#include "backends/duckdb/api/query_runtime.hpp"
#include "tpch/q18/duckdb/base/query.hpp"

#include <memory>

#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

Q18Parameters BindQ18Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q18 expects exactly 1 argument: quantity_threshold");
	}
	if (inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q18 quantity_threshold must be non-NULL");
	}
	return MakeQ18Parameters(inputs[0].GetValue<int64_t>());
}

QueryBindResult BindQ18Package(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ18Parameters(input.inputs);

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

duckdb::unique_ptr<KernelInstance> CreateQ18KernelInstance(const QueryParameters &parameters,
                                                           const QueryKernelFactoryData *) {
	auto &q18_parameters = dynamic_cast<const Q18Parameters &>(parameters);
	return CreateQ18Kernel(q18_parameters);
}

} // namespace

Q18Parameters MakeQ18Parameters(int64_t quantity_threshold) {
	Q18Parameters parameters;
	parameters.quantity_threshold = quantity_threshold;
	return parameters;
}

NativeScanSpec CreateQ18LineitemAggScanSpec(const Q18Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_quantity", "quantity", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	};
	return scan_spec;
}

NativeScanSpec CreateQ18CustomerScanSpec(const Q18Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "customer";
	scan_spec.projected_columns = {
	    {"c_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"c_name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

NativeScanSpec CreateQ18OrdersScanSpec(const Q18Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "orders";
	scan_spec.projected_columns = {
	    {"o_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_orderdate", "orderdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	    {"o_totalprice", "totalprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateTpchQ18FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q18";
	package.overloads = {
	    {{duckdb::LogicalType::BIGINT}},
	};
	package.bind_query = BindQ18Package;
	package.create_kernel = CreateQ18KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
