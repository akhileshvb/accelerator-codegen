#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q9/duckdb/base/query.hpp"

#include <memory>
#include <utility>

#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kLineitemDecimalWidth = 15;
constexpr uint8_t kLineitemDecimalScale = 2;
constexpr uint8_t kProfitDecimalWidth = 38;
constexpr uint8_t kProfitDecimalScale = 4;

Q9Parameters BindQ9Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q9 expects exactly 1 argument");
	}
	if (inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q9 color argument must not be NULL");
	}

	Q9Parameters parameters;
	parameters.color = inputs[0].GetValue<std::string>();
	return parameters;
}

QueryBindResult BindQ9Package(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ9Parameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q9Parameters>(parameters);
	result.input_scans.push_back(CreateQ9PartScanSpec(parameters));
	result.input_scans.push_back(CreateQ9SupplierScanSpec(parameters));
	result.input_scans.push_back(CreateQ9NationScanSpec(parameters));
	result.input_scans.push_back(CreateQ9OrdersScanSpec(parameters));
	result.input_scans.push_back(CreateQ9PartsuppScanSpec(parameters));
	result.input_scans.push_back(CreateQ9LineitemScanSpec(parameters));
	result.output_schema.names = {"nation", "o_year", "sum_profit"};
	result.output_schema.types = {
	    duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::BIGINT,
	    duckdb::LogicalType::DECIMAL(kProfitDecimalWidth, kProfitDecimalScale),
	};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ9KernelInstance(const QueryParameters &parameters,
                                                          const QueryKernelFactoryData *) {
	auto &q9_parameters = dynamic_cast<const Q9Parameters &>(parameters);
	return CreateQ9Kernel(q9_parameters);
}

} // namespace

NativeScanSpec CreateQ9PartScanSpec(const Q9Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "part";
	scan_spec.projected_columns = {
	    {"p_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"p_name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

NativeScanSpec CreateQ9SupplierScanSpec(const Q9Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "supplier";
	scan_spec.projected_columns = {
	    {"s_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"s_nationkey", "nationkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	return scan_spec;
}

NativeScanSpec CreateQ9NationScanSpec(const Q9Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "nation";
	scan_spec.projected_columns = {
	    {"n_nationkey", "nationkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	    {"n_name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

NativeScanSpec CreateQ9OrdersScanSpec(const Q9Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "orders";
	scan_spec.projected_columns = {
	    {"o_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_orderdate", "orderdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	};
	return scan_spec;
}

NativeScanSpec CreateQ9PartsuppScanSpec(const Q9Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "partsupp";
	scan_spec.projected_columns = {
	    {"ps_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"ps_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"ps_supplycost", "supplycost", duckdb::LogicalType::DECIMAL(kLineitemDecimalWidth, kLineitemDecimalScale),
	     duckdb::PhysicalType::INT64},
	};
	return scan_spec;
}

NativeScanSpec CreateQ9LineitemScanSpec(const Q9Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_quantity", "quantity", duckdb::LogicalType::DECIMAL(kLineitemDecimalWidth, kLineitemDecimalScale),
	     duckdb::PhysicalType::INT64},
	    {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(kLineitemDecimalWidth, kLineitemDecimalScale),
	     duckdb::PhysicalType::INT64},
	    {"l_discount", "discount", duckdb::LogicalType::DECIMAL(kLineitemDecimalWidth, kLineitemDecimalScale),
	     duckdb::PhysicalType::INT64},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateTpchQ9FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q9";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ9Package;
	package.create_kernel = CreateQ9KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
