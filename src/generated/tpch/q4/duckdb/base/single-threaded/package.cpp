#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q4/duckdb/base/query.hpp"

#include <memory>

#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

duckdb::date_t ComputeExclusiveEndDate(duckdb::date_t start_date) {
	duckdb::interval_t three_months;
	three_months.months = 3;
	three_months.days = 0;
	three_months.micros = 0;
	return duckdb::Interval::Add(start_date, three_months);
}

Q4Parameters BindQ4Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q4 expects exactly 1 argument");
	}
	return MakeQ4Parameters(inputs[0].GetValue<duckdb::date_t>());
}

QueryBindResult BindQ4Package(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ4Parameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q4Parameters>(parameters);
	result.input_scans.push_back(CreateQ4LineitemScanSpec(parameters));
	result.input_scans.push_back(CreateQ4OrdersScanSpec(parameters));
	result.output_schema.names = {"o_orderpriority", "order_count"};
	result.output_schema.types = {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ4KernelInstance(const QueryParameters &parameters,
                                                          const QueryKernelFactoryData *) {
	auto &q4_parameters = dynamic_cast<const Q4Parameters &>(parameters);
	return CreateQ4Kernel(q4_parameters);
}

} // namespace

Q4Parameters MakeQ4Parameters(duckdb::date_t start_date) {
	Q4Parameters parameters;
	parameters.start_date = start_date;
	parameters.end_date = ComputeExclusiveEndDate(start_date);
	return parameters;
}

NativeScanSpec CreateQ4LineitemScanSpec(const Q4Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_commitdate", "commitdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	    {"l_receiptdate", "receiptdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	};
	return scan_spec;
}

NativeScanSpec CreateQ4OrdersScanSpec(const Q4Parameters &parameters) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "orders";
	scan_spec.projected_columns = {
	    {"o_orderdate", "orderdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	    {"o_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_orderpriority", "orderpriority", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	scan_spec.filters = {
	    {"o_orderdate", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
	     duckdb::Value::DATE(parameters.start_date)},
	    {"o_orderdate", duckdb::ExpressionType::COMPARE_LESSTHAN, duckdb::Value::DATE(parameters.end_date)},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateTpchQ4FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q4";
	package.overloads = {
	    {{duckdb::LogicalType::DATE}},
	};
	package.bind_query = BindQ4Package;
	package.create_kernel = CreateQ4KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
