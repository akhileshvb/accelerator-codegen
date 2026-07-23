#include "accelerator_codegen/query_runtime.hpp"
#include "accelerator_codegen/tpch_q11.hpp"

#include <memory>
#include <sstream>

#include "duckdb/common/string_util.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/connection.hpp"

namespace accelerator_codegen {
namespace {

Q11Parameters BindQ11ParallelParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 2) {
		throw duckdb::BinderException("accel_tpch_q11_parallel expects exactly 2 arguments: nation_name, fraction");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q11_parallel arguments must be non-NULL");
	}
	return MakeQ11Parameters(inputs[0].GetValue<std::string>(), inputs[1].GetValue<double>());
}

int32_t ResolveNationKey(duckdb::ClientContext &context, const std::string &nation_name) {
	duckdb::Connection connection(*context.db);
	std::ostringstream sql;
	sql << "SELECT n_nationkey FROM nation WHERE n_name = '"
	    << duckdb::StringUtil::Replace(nation_name, "'", "''") << "'";
	auto result = connection.Query(sql.str());
	if (result->HasError()) {
		throw duckdb::BinderException("failed to resolve nation name \"%s\": %s", nation_name, result->GetError());
	}
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	if (materialized.RowCount() != 1) {
		throw duckdb::BinderException("nation name \"%s\" did not resolve to exactly one nation key", nation_name);
	}
	return materialized.GetValue(0, 0).GetValue<int32_t>();
}

QueryBindResult BindQ11ParallelPackage(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ11ParallelParameters(input.inputs);
	parameters.target_nationkey = ResolveNationKey(context, parameters.nation_name);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q11Parameters>(parameters);
	result.input_scans.push_back(CreateQ11SupplierScanSpec(parameters));
	result.input_scans.push_back(CreateQ11PartsuppScanSpec(parameters));
	result.output_schema.names = {"ps_partkey", "value"};
	result.output_schema.types = {duckdb::LogicalType::BIGINT, duckdb::LogicalType::DECIMAL(38, 2)};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ11ParallelKernelInstance(const QueryParameters &parameters,
                                                                   const QueryKernelFactoryData *) {
	auto &q11_parameters = dynamic_cast<const Q11Parameters &>(parameters);
	return CreateQ11ParallelKernel(q11_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ11ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q11_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::DOUBLE}},
	};
	package.bind_query = BindQ11ParallelPackage;
	package.create_kernel = CreateQ11ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
