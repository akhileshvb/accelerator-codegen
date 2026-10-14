#include "backends/duckdb/api/query_runtime.hpp"
#include "tpch/q10/duckdb/base/query.hpp"

#include <memory>

#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;

Q10Parameters BindQ10ParallelParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q10_parallel expects exactly 1 argument: start_date");
	}
	if (inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q10_parallel start_date must be non-NULL");
	}
	return MakeQ10Parameters(inputs[0].GetValue<duckdb::date_t>());
}

QueryBindResult BindQ10ParallelPackage(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ10ParallelParameters(input.inputs);

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

duckdb::unique_ptr<KernelInstance> CreateQ10ParallelKernelInstance(const QueryParameters &parameters,
                                                                   const QueryKernelFactoryData *) {
	auto &q10_parameters = dynamic_cast<const Q10Parameters &>(parameters);
	return CreateQ10ParallelKernel(q10_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ10ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q10_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::DATE}},
	};
	package.bind_query = BindQ10ParallelPackage;
	package.create_kernel = CreateQ10ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
