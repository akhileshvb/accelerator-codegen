#include "backends/duckdb/api/query_runtime.hpp"
#include "tpch/q5/duckdb/base/query.hpp"

#include <memory>

#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;

duckdb::date_t ComputeExclusiveEndDate(duckdb::date_t start_date) {
	duckdb::interval_t one_year;
	one_year.months = 12;
	one_year.days = 0;
	one_year.micros = 0;
	return duckdb::Interval::Add(start_date, one_year);
}

Q5Parameters BindQ5Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 2) {
		throw duckdb::BinderException("accel_tpch_q5 expects exactly 2 arguments: region_name, start_date");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q5 arguments must be non-NULL");
	}
	return MakeQ5Parameters(inputs[0].GetValue<std::string>(), inputs[1].GetValue<duckdb::date_t>());
}

QueryBindResult BindQ5Package(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ5Parameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q5Parameters>(parameters);
	result.input_scans.push_back(CreateQ5RegionScanSpec(parameters));
	result.input_scans.push_back(CreateQ5NationScanSpec(parameters));
	result.input_scans.push_back(CreateQ5CustomerScanSpec(parameters));
	result.input_scans.push_back(CreateQ5SupplierScanSpec(parameters));
	result.input_scans.push_back(CreateQ5OrdersScanSpec(parameters));
	result.input_scans.push_back(CreateQ5LineitemScanSpec(parameters));
	result.output_schema.names = {"n_name", "revenue"};
	result.output_schema.types = {duckdb::LogicalType::VARCHAR,
	                              duckdb::LogicalType::DECIMAL(kRevenueDecimalWidth, kRevenueDecimalScale)};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ5KernelInstance(const QueryParameters &parameters,
                                                         const QueryKernelFactoryData *) {
	auto &q5_parameters = dynamic_cast<const Q5Parameters &>(parameters);
	return CreateQ5Kernel(q5_parameters);
}

} // namespace

Q5Parameters MakeQ5Parameters(const std::string &region_name, duckdb::date_t start_date) {
	Q5Parameters parameters;
	parameters.region_name = region_name;
	parameters.start_date = start_date;
	parameters.end_date = ComputeExclusiveEndDate(start_date);
	return parameters;
}

NativeScanSpec CreateQ5RegionScanSpec(const Q5Parameters &parameters) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "region";
	scan_spec.projected_columns = {
	    {"r_regionkey", "regionkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	    {"r_name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	scan_spec.filters = {
	    {"r_name", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value(parameters.region_name)},
	};
	return scan_spec;
}

NativeScanSpec CreateQ5NationScanSpec(const Q5Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "nation";
	scan_spec.projected_columns = {
	    {"n_nationkey", "nationkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	    {"n_name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"n_regionkey", "regionkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	return scan_spec;
}

NativeScanSpec CreateQ5CustomerScanSpec(const Q5Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "customer";
	scan_spec.projected_columns = {
	    {"c_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"c_nationkey", "nationkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	return scan_spec;
}

NativeScanSpec CreateQ5SupplierScanSpec(const Q5Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "supplier";
	scan_spec.projected_columns = {
	    {"s_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"s_nationkey", "nationkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	return scan_spec;
}

NativeScanSpec CreateQ5OrdersScanSpec(const Q5Parameters &parameters) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "orders";
	scan_spec.projected_columns = {
	    {"o_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_orderdate", "orderdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	};
	scan_spec.filters = {
	    {"o_orderdate", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
	     duckdb::Value::DATE(parameters.start_date)},
	    {"o_orderdate", duckdb::ExpressionType::COMPARE_LESSTHAN, duckdb::Value::DATE(parameters.end_date)},
	};
	return scan_spec;
}

NativeScanSpec CreateQ5LineitemScanSpec(const Q5Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_discount", "discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateTpchQ5FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q5";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::DATE}},
	};
	package.bind_query = BindQ5Package;
	package.create_kernel = CreateQ5KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
