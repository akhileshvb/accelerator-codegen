#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q19/base/query.hpp"

#include <memory>

#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

Q19Parameters BindQ19Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 6) {
		throw duckdb::BinderException(
		    "accel_tpch_q19 expects exactly 6 arguments: brand1, qty_low1, brand2, qty_low2, brand3, qty_low3");
	}
	for (duckdb::idx_t i = 0; i < inputs.size(); i++) {
		if (inputs[i].IsNull()) {
			throw duckdb::BinderException("accel_tpch_q19 arguments must be non-NULL");
		}
	}
	return MakeQ19Parameters(inputs[0].GetValue<std::string>(), inputs[1].GetValue<int64_t>(),
	                           inputs[2].GetValue<std::string>(), inputs[3].GetValue<int64_t>(),
	                           inputs[4].GetValue<std::string>(), inputs[5].GetValue<int64_t>());
}

QueryBindResult BindQ19Package(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ19Parameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q19Parameters>(parameters);
	result.input_scans.push_back(CreateQ19PartScanSpec(parameters));
	result.input_scans.push_back(CreateQ19LineitemScanSpec(parameters));
	result.output_schema.names = {"revenue"};
	result.output_schema.types = {duckdb::LogicalType::DECIMAL(38, 4)};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ19KernelInstance(const QueryParameters &parameters,
                                                         const QueryKernelFactoryData *) {
	auto &q19_parameters = dynamic_cast<const Q19Parameters &>(parameters);
	return CreateQ19Kernel(q19_parameters);
}

} // namespace

Q19Parameters MakeQ19Parameters(const std::string &brand1, int64_t qty_low1, const std::string &brand2,
                                int64_t qty_low2, const std::string &brand3, int64_t qty_low3) {
	Q19Parameters parameters;
	parameters.brand1 = brand1;
	parameters.qty_low1 = qty_low1;
	parameters.brand2 = brand2;
	parameters.qty_low2 = qty_low2;
	parameters.brand3 = brand3;
	parameters.qty_low3 = qty_low3;
	return parameters;
}

NativeScanSpec CreateQ19PartScanSpec(const Q19Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "part";
	scan_spec.projected_columns = {
	    {"p_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"p_brand", "brand", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"p_container", "container", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"p_size", "size", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	return scan_spec;
}

NativeScanSpec CreateQ19LineitemScanSpec(const Q19Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_quantity", "quantity", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_discount", "discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_shipmode", "shipmode", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"l_shipinstruct", "shipinstruct", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateTpchQ19FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q19";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT, duckdb::LogicalType::VARCHAR,
	      duckdb::LogicalType::BIGINT, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT}},
	};
	package.bind_query = BindQ19Package;
	package.create_kernel = CreateQ19KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
