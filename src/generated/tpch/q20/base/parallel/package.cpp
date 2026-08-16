#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q20/base/query.hpp"

#include <memory>
#include <sstream>
#include <utility>

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/connection.hpp"

namespace accelerator_codegen {
namespace {

Q20Parameters BindQ20ParallelParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 3) {
		throw duckdb::BinderException(
		    "accel_tpch_q20_parallel expects exactly 3 arguments: start_date, part_name_pattern, nation_name");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull() || inputs[2].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q20_parallel arguments must be non-NULL");
	}
	return MakeQ20Parameters(inputs[0].GetValue<duckdb::date_t>(), inputs[1].GetValue<std::string>(),
	                         inputs[2].GetValue<std::string>());
}

int32_t ResolveNationKey(duckdb::ClientContext &context, const std::string &nation_name) {
	duckdb::Connection connection(*context.db);
	std::ostringstream sql;
	sql << "SELECT DISTINCT n_nationkey FROM nation WHERE n_name = '"
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

QueryBindResult BindQ20ParallelPackage(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ20ParallelParameters(input.inputs);
	parameters.target_nationkey = ResolveNationKey(context, parameters.nation_name);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q20Parameters>(parameters);
	result.input_scans.push_back(CreateQ20PartScanSpec(parameters));
	result.input_scans.push_back(CreateQ20LineitemScanSpec(parameters));
	result.input_scans.push_back(CreateQ20PartsuppScanSpec(parameters));
	result.input_scans.push_back(CreateQ20SupplierScanSpec(parameters));
	result.output_schema.names = {"s_name", "s_address"};
	result.output_schema.types = {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ20ParallelKernelInstance(const QueryParameters &parameters,
                                                                   const QueryKernelFactoryData *) {
	auto &q20_parameters = dynamic_cast<const Q20Parameters &>(parameters);
	return CreateQ20ParallelKernel(q20_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ20ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q20_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::DATE, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ20ParallelPackage;
	package.create_kernel = CreateQ20ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
