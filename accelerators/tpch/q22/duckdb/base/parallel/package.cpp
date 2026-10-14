#include "backends/duckdb/api/query_runtime.hpp"
#include "tpch/q22/duckdb/base/query.hpp"

#include <memory>

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

Q22Parameters BindQ22ParallelParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 7) {
		throw duckdb::BinderException(
		    "accel_tpch_q22_parallel expects exactly 7 arguments: country_code1..country_code7");
	}
	std::array<std::string, 7> country_codes;
	for (duckdb::idx_t index = 0; index < 7; index++) {
		if (inputs[index].IsNull()) {
			throw duckdb::BinderException("accel_tpch_q22_parallel arguments must be non-NULL");
		}
		country_codes[index] = inputs[index].GetValue<std::string>();
	}
	return MakeQ22Parameters(country_codes);
}

QueryBindResult BindQ22ParallelPackage(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ22ParallelParameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q22Parameters>(parameters);
	result.input_scans.push_back(CreateQ22OrdersScanSpec(parameters));
	result.input_scans.push_back(CreateQ22CustomerScanSpec(parameters));
	result.output_schema.names = {"cntrycode", "numcust", "totacctbal"};
	result.output_schema.types = {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT,
	                              duckdb::LogicalType::DECIMAL(38, 2)};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ22ParallelKernelInstance(const QueryParameters &parameters,
                                                                   const QueryKernelFactoryData *) {
	auto &q22_parameters = dynamic_cast<const Q22Parameters &>(parameters);
	return CreateQ22ParallelKernel(q22_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ22ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q22_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR,
	      duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR,
	      duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ22ParallelPackage;
	package.create_kernel = CreateQ22ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
