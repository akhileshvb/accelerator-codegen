#include "accelerator_codegen/query_runtime.hpp"
#include "accelerator_codegen/tpch_q21.hpp"

#include <memory>
#include <sstream>

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/connection.hpp"

namespace accelerator_codegen {
namespace {

Q21Parameters BindQ21ParallelParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q21_parallel expects exactly 1 argument: nation_name");
	}
	if (inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q21_parallel nation_name must be non-NULL");
	}
	return MakeQ21Parameters(inputs[0].GetValue<std::string>());
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

QueryBindResult BindQ21ParallelPackage(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ21ParallelParameters(input.inputs);
	parameters.target_nationkey = ResolveNationKey(context, parameters.nation_name);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q21Parameters>(parameters);
	result.input_scans.push_back(CreateQ21SupplierScanSpec(parameters));
	result.input_scans.push_back(CreateQ21OrdersScanSpec(parameters));
	result.input_scans.push_back(CreateQ21LineitemScanSpec(parameters));
	result.output_schema.names = {"s_name", "numwait"};
	result.output_schema.types = {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ21ParallelKernelInstance(const QueryParameters &parameters,
                                                                   const QueryKernelFactoryData *) {
	auto &q21_parameters = dynamic_cast<const Q21Parameters &>(parameters);
	return CreateQ21ParallelKernel(q21_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ21ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q21_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ21ParallelPackage;
	package.create_kernel = CreateQ21ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
