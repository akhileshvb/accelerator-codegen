#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q22/duckdb/base/query.hpp"

#include <memory>

#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

Q22Parameters BindQ22Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 7) {
		throw duckdb::BinderException("accel_tpch_q22 expects exactly 7 arguments: country_code1..country_code7");
	}
	std::array<std::string, 7> country_codes;
	for (duckdb::idx_t index = 0; index < 7; index++) {
		if (inputs[index].IsNull()) {
			throw duckdb::BinderException("accel_tpch_q22 arguments must be non-NULL");
		}
		country_codes[index] = inputs[index].GetValue<std::string>();
	}
	return MakeQ22Parameters(country_codes);
}

QueryBindResult BindQ22Package(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ22Parameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q22Parameters>(parameters);
	result.input_scans.push_back(CreateQ22OrdersScanSpec(parameters));
	result.input_scans.push_back(CreateQ22CustomerScanSpec(parameters));
	result.output_schema.names = {"cntrycode", "numcust", "totacctbal"};
	result.output_schema.types = {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT,
	                              duckdb::LogicalType::DECIMAL(38, 2)};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ22KernelInstance(const QueryParameters &parameters,
                                                           const QueryKernelFactoryData *) {
	auto &q22_parameters = dynamic_cast<const Q22Parameters &>(parameters);
	return CreateQ22Kernel(q22_parameters);
}

} // namespace

Q22Parameters MakeQ22Parameters(const std::array<std::string, 7> &country_codes) {
	Q22Parameters parameters;
	parameters.country_codes = country_codes;
	return parameters;
}

NativeScanSpec CreateQ22OrdersScanSpec(const Q22Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "orders";
	scan_spec.projected_columns = {
	    {"o_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	};
	return scan_spec;
}

NativeScanSpec CreateQ22CustomerScanSpec(const Q22Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "customer";
	scan_spec.projected_columns = {
	    {"c_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"c_phone", "phone", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"c_acctbal", "acctbal", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateTpchQ22FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q22";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR,
	      duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR,
	      duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ22Package;
	package.create_kernel = CreateQ22KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
