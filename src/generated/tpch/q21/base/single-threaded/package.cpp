#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q21/base/query.hpp"

#include <memory>
#include <sstream>

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/connection.hpp"

namespace accelerator_codegen {
namespace {

Q21Parameters BindQ21Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q21 expects exactly 1 argument: nation_name");
	}
	if (inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q21 nation_name must be non-NULL");
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

QueryBindResult BindQ21Package(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ21Parameters(input.inputs);
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

duckdb::unique_ptr<KernelInstance> CreateQ21KernelInstance(const QueryParameters &parameters,
                                                           const QueryKernelFactoryData *) {
	auto &q21_parameters = dynamic_cast<const Q21Parameters &>(parameters);
	return CreateQ21Kernel(q21_parameters);
}

} // namespace

Q21Parameters MakeQ21Parameters(const std::string &nation_name) {
	Q21Parameters parameters;
	parameters.nation_name = nation_name;
	return parameters;
}

NativeScanSpec CreateQ21SupplierScanSpec(const Q21Parameters &parameters) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "supplier";
	scan_spec.projected_columns = {
	    {"s_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"s_name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"s_nationkey", "nationkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	if (parameters.target_nationkey >= 0) {
		scan_spec.filters = {
		    {"s_nationkey", duckdb::ExpressionType::COMPARE_EQUAL,
		     duckdb::Value::INTEGER(parameters.target_nationkey)},
		};
	}
	return scan_spec;
}

NativeScanSpec CreateQ21OrdersScanSpec(const Q21Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "orders";
	scan_spec.projected_columns = {
	    {"o_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_orderstatus", "orderstatus", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	scan_spec.filters = {
	    {"o_orderstatus", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value("F")},
	};
	return scan_spec;
}

NativeScanSpec CreateQ21LineitemScanSpec(const Q21Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_commitdate", "commitdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	    {"l_receiptdate", "receiptdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateTpchQ21FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q21";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ21Package;
	package.create_kernel = CreateQ21KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
