#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q10/duckdb/base/query.hpp"

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

Q10Parameters BindQ10Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q10 expects exactly 1 argument: start_date");
	}
	if (inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q10 start_date must be non-NULL");
	}
	return MakeQ10Parameters(inputs[0].GetValue<duckdb::date_t>());
}

QueryBindResult BindQ10Package(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ10Parameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q10Parameters>(parameters);
	result.input_scans.push_back(CreateQ10NationScanSpec(parameters));
	result.input_scans.push_back(CreateQ10OrdersScanSpec(parameters));
	result.input_scans.push_back(CreateQ10LineitemScanSpec(parameters));
	result.input_scans.push_back(CreateQ10CustomerScanSpec(parameters));
	result.output_schema.names = {"c_custkey", "c_name", "revenue", "c_acctbal", "n_name", "c_address", "c_phone",
	                              "c_comment"};
	result.output_schema.types = {duckdb::LogicalType::BIGINT,
	                              duckdb::LogicalType::VARCHAR,
	                              duckdb::LogicalType::DECIMAL(kRevenueDecimalWidth, kRevenueDecimalScale),
	                              duckdb::LogicalType::DECIMAL(15, 2),
	                              duckdb::LogicalType::VARCHAR,
	                              duckdb::LogicalType::VARCHAR,
	                              duckdb::LogicalType::VARCHAR,
	                              duckdb::LogicalType::VARCHAR};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ10KernelInstance(const QueryParameters &parameters,
                                                           const QueryKernelFactoryData *) {
	auto &q10_parameters = dynamic_cast<const Q10Parameters &>(parameters);
	return CreateQ10Kernel(q10_parameters);
}

} // namespace

Q10Parameters MakeQ10Parameters(duckdb::date_t start_date) {
	Q10Parameters parameters;
	parameters.start_date = start_date;
	parameters.end_date = ComputeExclusiveEndDate(start_date);
	return parameters;
}

NativeScanSpec CreateQ10CustomerScanSpec(const Q10Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "customer";
	scan_spec.projected_columns = {
	    {"c_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"c_name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"c_acctbal", "acctbal", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"c_nationkey", "nationkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	    {"c_address", "address", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"c_phone", "phone", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"c_comment", "comment", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

NativeScanSpec CreateQ10NationScanSpec(const Q10Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "nation";
	scan_spec.projected_columns = {
	    {"n_nationkey", "nationkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	    {"n_name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

NativeScanSpec CreateQ10OrdersScanSpec(const Q10Parameters &parameters) {
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

NativeScanSpec CreateQ10LineitemScanSpec(const Q10Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_discount", "discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	};
	scan_spec.filters = {
	    {"l_returnflag", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value("R")},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateTpchQ10FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q10";
	package.overloads = {
	    {{duckdb::LogicalType::DATE}},
	};
	package.bind_query = BindQ10Package;
	package.create_kernel = CreateQ10KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
