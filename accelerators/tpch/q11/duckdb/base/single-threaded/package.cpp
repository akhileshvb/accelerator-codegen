#include "backends/duckdb/api/query_runtime.hpp"
#include "tpch/q11/duckdb/base/query.hpp"

#include <memory>
#include <sstream>

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/connection.hpp"

namespace accelerator_codegen {
namespace {

Q11Parameters BindQ11Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 2) {
		throw duckdb::BinderException("accel_tpch_q11 expects exactly 2 arguments: nation_name, fraction");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q11 arguments must be non-NULL");
	}
	return MakeQ11Parameters(inputs[0].GetValue<std::string>(), inputs[1].GetValue<double>());
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

QueryBindResult BindQ11Package(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ11Parameters(input.inputs);
	parameters.target_nationkey = ResolveNationKey(context, parameters.nation_name);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q11Parameters>(parameters);
	result.input_scans.push_back(CreateQ11SupplierScanSpec(parameters));
	result.input_scans.push_back(CreateQ11PartsuppScanSpec(parameters));
	result.output_schema.names = {"ps_partkey", "value"};
	result.output_schema.types = {duckdb::LogicalType::BIGINT, duckdb::LogicalType::DECIMAL(38, 2)};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ11KernelInstance(const QueryParameters &parameters,
                                                         const QueryKernelFactoryData *) {
	auto &q11_parameters = dynamic_cast<const Q11Parameters &>(parameters);
	return CreateQ11Kernel(q11_parameters);
}

} // namespace

Q11Parameters MakeQ11Parameters(const std::string &nation_name, double fraction) {
	Q11Parameters parameters;
	parameters.nation_name = nation_name;
	parameters.fraction = fraction;
	return parameters;
}

NativeScanSpec CreateQ11NationScanSpec(const Q11Parameters &parameters) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "nation";
	scan_spec.projected_columns = {
	    {"n_nationkey", "nationkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	    {"n_name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	scan_spec.filters = {
	    {"n_name", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value(parameters.nation_name)},
	};
	return scan_spec;
}

NativeScanSpec CreateQ11SupplierScanSpec(const Q11Parameters &parameters) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "supplier";
	scan_spec.projected_columns = {
	    {"s_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
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

NativeScanSpec CreateQ11PartsuppScanSpec(const Q11Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "partsupp";
	scan_spec.projected_columns = {
	    {"ps_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"ps_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"ps_availqty", "availqty", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"ps_supplycost", "supplycost", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateTpchQ11FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q11";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::DOUBLE}},
	};
	package.bind_query = BindQ11Package;
	package.create_kernel = CreateQ11KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
