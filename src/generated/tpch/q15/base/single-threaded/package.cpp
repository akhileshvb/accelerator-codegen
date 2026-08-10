#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q15/base/query.hpp"

#include <memory>

#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;

duckdb::date_t ComputeExclusiveEndDate(duckdb::date_t start_date) {
	duckdb::interval_t three_months;
	three_months.months = 3;
	three_months.days = 0;
	three_months.micros = 0;
	return duckdb::Interval::Add(start_date, three_months);
}

Q15Parameters BindQ15Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q15 expects exactly 1 argument: start_date");
	}
	if (inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q15 start_date must be non-NULL");
	}
	return MakeQ15Parameters(inputs[0].GetValue<duckdb::date_t>());
}

QueryBindResult BindQ15Package(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ15Parameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q15Parameters>(parameters);
	result.input_scans.push_back(CreateQ15LineitemScanSpec(parameters));
	result.input_scans.push_back(CreateQ15SupplierScanSpec(parameters));
	result.output_schema.names = {"s_suppkey", "s_name", "s_address", "s_phone", "total_revenue"};
	result.output_schema.types = {duckdb::LogicalType::BIGINT, duckdb::LogicalType::VARCHAR,
	                              duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR,
	                              duckdb::LogicalType::DECIMAL(kRevenueDecimalWidth, kRevenueDecimalScale)};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ15KernelInstance(const QueryParameters &parameters,
                                                           const QueryKernelFactoryData *) {
	auto &q15_parameters = dynamic_cast<const Q15Parameters &>(parameters);
	return CreateQ15Kernel(q15_parameters);
}

} // namespace

Q15Parameters MakeQ15Parameters(duckdb::date_t start_date) {
	Q15Parameters parameters;
	parameters.start_date = start_date;
	parameters.end_date = ComputeExclusiveEndDate(start_date);
	return parameters;
}

NativeScanSpec CreateQ15LineitemScanSpec(const Q15Parameters &parameters) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	    {"l_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_discount", "discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	};
	scan_spec.filters = {
	    {"l_shipdate", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
	     duckdb::Value::DATE(parameters.start_date)},
	    {"l_shipdate", duckdb::ExpressionType::COMPARE_LESSTHAN, duckdb::Value::DATE(parameters.end_date)},
	};
	return scan_spec;
}

NativeScanSpec CreateQ15SupplierScanSpec(const Q15Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "supplier";
	scan_spec.projected_columns = {
	    {"s_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"s_name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"s_address", "address", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"s_phone", "phone", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateTpchQ15FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q15";
	package.overloads = {
	    {{duckdb::LogicalType::DATE}},
	};
	package.bind_query = BindQ15Package;
	package.create_kernel = CreateQ15KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
