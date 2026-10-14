#include "backends/duckdb/api/query_runtime.hpp"
#include "tpch/q16/duckdb/base/query.hpp"

#include <memory>

#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

Q16Parameters BindQ16Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 10) {
		throw duckdb::BinderException(
		    "accel_tpch_q16 expects exactly 10 arguments: brand, type_pattern, size1..size8");
	}
	for (const auto &input : inputs) {
		if (input.IsNull()) {
			throw duckdb::BinderException("accel_tpch_q16 arguments must be non-NULL");
		}
	}
	std::array<int32_t, 8> sizes;
	for (duckdb::idx_t index = 0; index < 8; index++) {
		sizes[index] = inputs[2 + index].GetValue<int32_t>();
	}
	return MakeQ16Parameters(inputs[0].GetValue<std::string>(), inputs[1].GetValue<std::string>(), sizes);
}

QueryBindResult BindQ16Package(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ16Parameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q16Parameters>(parameters);
	result.input_scans.push_back(CreateQ16SupplierScanSpec(parameters));
	result.input_scans.push_back(CreateQ16PartScanSpec(parameters));
	result.input_scans.push_back(CreateQ16PartsuppScanSpec(parameters));
	result.output_schema.names = {"p_brand", "p_type", "p_size", "supplier_cnt"};
	result.output_schema.types = {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR,
	                              duckdb::LogicalType::INTEGER, duckdb::LogicalType::BIGINT};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ16KernelInstance(const QueryParameters &parameters,
                                                           const QueryKernelFactoryData *) {
	auto &q16_parameters = dynamic_cast<const Q16Parameters &>(parameters);
	return CreateQ16Kernel(q16_parameters);
}

} // namespace

Q16Parameters MakeQ16Parameters(const std::string &brand, const std::string &type_pattern,
                                const std::array<int32_t, 8> &sizes) {
	Q16Parameters parameters;
	parameters.brand = brand;
	parameters.type_pattern = type_pattern;
	parameters.sizes = sizes;
	return parameters;
}

NativeScanSpec CreateQ16SupplierScanSpec(const Q16Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "supplier";
	scan_spec.projected_columns = {
	    {"s_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"s_comment", "comment", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

NativeScanSpec CreateQ16PartScanSpec(const Q16Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "part";
	scan_spec.projected_columns = {
	    {"p_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"p_brand", "brand", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"p_type", "type", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"p_size", "size", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	return scan_spec;
}

NativeScanSpec CreateQ16PartsuppScanSpec(const Q16Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "partsupp";
	scan_spec.projected_columns = {
	    {"ps_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"ps_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateTpchQ16FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q16";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::INTEGER,
	      duckdb::LogicalType::INTEGER, duckdb::LogicalType::INTEGER, duckdb::LogicalType::INTEGER,
	      duckdb::LogicalType::INTEGER, duckdb::LogicalType::INTEGER, duckdb::LogicalType::INTEGER,
	      duckdb::LogicalType::INTEGER}},
	};
	package.bind_query = BindQ16Package;
	package.create_kernel = CreateQ16KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
