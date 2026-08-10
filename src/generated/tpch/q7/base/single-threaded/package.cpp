#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q7/base/query.hpp"

#include <memory>

#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;

Q7Parameters BindQ7Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 2) {
		throw duckdb::BinderException("accel_tpch_q7 expects exactly 2 arguments: nation1, nation2");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q7 arguments must be non-NULL");
	}
	return MakeQ7Parameters(inputs[0].GetValue<std::string>(), inputs[1].GetValue<std::string>());
}

QueryBindResult BindQ7Package(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ7Parameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q7Parameters>(parameters);
	result.input_scans.push_back(CreateQ7NationScanSpec(parameters));
	result.input_scans.push_back(CreateQ7CustomerScanSpec(parameters));
	result.input_scans.push_back(CreateQ7SupplierScanSpec(parameters));
	result.input_scans.push_back(CreateQ7OrdersScanSpec(parameters));
	result.input_scans.push_back(CreateQ7LineitemScanSpec(parameters));
	result.output_schema.names = {"supp_nation", "cust_nation", "l_year", "revenue"};
	result.output_schema.types = {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT,
	                              duckdb::LogicalType::DECIMAL(kRevenueDecimalWidth, kRevenueDecimalScale)};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ7KernelInstance(const QueryParameters &parameters,
                                                         const QueryKernelFactoryData *) {
	auto &q7_parameters = dynamic_cast<const Q7Parameters &>(parameters);
	return CreateQ7Kernel(q7_parameters);
}

} // namespace

Q7Parameters MakeQ7Parameters(const std::string &nation1, const std::string &nation2) {
	Q7Parameters parameters;
	parameters.nation1 = nation1;
	parameters.nation2 = nation2;
	parameters.shipdate_start = duckdb::Date::FromString("1995-01-01");
	parameters.shipdate_end = duckdb::Date::FromString("1996-12-31");
	return parameters;
}

NativeScanSpec CreateQ7NationScanSpec(const Q7Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "nation";
	scan_spec.projected_columns = {
	    {"n_nationkey", "nationkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	    {"n_name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

NativeScanSpec CreateQ7CustomerScanSpec(const Q7Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "customer";
	scan_spec.projected_columns = {
	    {"c_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"c_nationkey", "nationkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	return scan_spec;
}

NativeScanSpec CreateQ7SupplierScanSpec(const Q7Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "supplier";
	scan_spec.projected_columns = {
	    {"s_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"s_nationkey", "nationkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	return scan_spec;
}

NativeScanSpec CreateQ7OrdersScanSpec(const Q7Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "orders";
	scan_spec.projected_columns = {
	    {"o_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	};
	return scan_spec;
}

NativeScanSpec CreateQ7LineitemScanSpec(const Q7Parameters &parameters) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	    {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_discount", "discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	};
	scan_spec.filters = {
	    {"l_shipdate", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
	     duckdb::Value::DATE(parameters.shipdate_start)},
	    {"l_shipdate", duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO,
	     duckdb::Value::DATE(parameters.shipdate_end)},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateTpchQ7FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q7";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ7Package;
	package.create_kernel = CreateQ7KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
