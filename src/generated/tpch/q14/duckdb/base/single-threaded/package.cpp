#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q14/duckdb/base/query.hpp"

#include <memory>

#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

duckdb::date_t ComputeExclusiveEndDate(duckdb::date_t start_date) {
	duckdb::interval_t one_month;
	one_month.months = 1;
	one_month.days = 0;
	one_month.micros = 0;
	return duckdb::Interval::Add(start_date, one_month);
}

Q14Parameters BindQ14Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q14 expects exactly 1 argument: start_date");
	}
	if (inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q14 start_date must be non-NULL");
	}
	return MakeQ14Parameters(inputs[0].GetValue<duckdb::date_t>());
}

QueryBindResult BindQ14Package(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ14Parameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q14Parameters>(parameters);
	result.input_scans.push_back(CreateQ14PartScanSpec(parameters));
	result.input_scans.push_back(CreateQ14LineitemScanSpec(parameters));
	// DuckDB evaluates 100.00 * DECIMAL / DECIMAL as DOUBLE for the reference Q14 SQL.
	result.output_schema.names = {"promo_revenue"};
	result.output_schema.types = {duckdb::LogicalType::DOUBLE};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ14KernelInstance(const QueryParameters &parameters,
                                                           const QueryKernelFactoryData *) {
	auto &q14_parameters = dynamic_cast<const Q14Parameters &>(parameters);
	return CreateQ14Kernel(q14_parameters);
}

} // namespace

Q14Parameters MakeQ14Parameters(duckdb::date_t start_date) {
	Q14Parameters parameters;
	parameters.start_date = start_date;
	parameters.end_date = ComputeExclusiveEndDate(start_date);
	return parameters;
}

NativeScanSpec CreateQ14PartScanSpec(const Q14Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "part";
	scan_spec.projected_columns = {
	    {"p_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"p_type", "type", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

NativeScanSpec CreateQ14LineitemScanSpec(const Q14Parameters &parameters) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_discount", "discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	};
	// Push the half-open ship-date window; residual re-check remains for txn-local FLAT vectors.
	scan_spec.filters = {
	    {"l_shipdate", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
	     duckdb::Value::DATE(parameters.start_date)},
	    {"l_shipdate", duckdb::ExpressionType::COMPARE_LESSTHAN, duckdb::Value::DATE(parameters.end_date)},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateTpchQ14FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q14";
	package.overloads = {
	    {{duckdb::LogicalType::DATE}},
	};
	package.bind_query = BindQ14Package;
	package.create_kernel = CreateQ14KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
