#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "sec_edgar/q24/duckdb/base/query.hpp"
#include "sec_edgar/q24/duckdb/base/fused.hpp"

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

QueryBindResult BindSecEdgarQ24Package(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &) {
	SecEdgarQ24Parameters parameters;
	parameters.row_estimate = GetNativeScanTableRowCount(context, CreateSecEdgarQ24NumScanSpec());

	QueryBindResult result;
	result.query_parameters = std::make_unique<SecEdgarQ24Parameters>(parameters);
	result.input_scans.push_back(CreateSecEdgarQ24NumScanSpec());
	result.output_schema.names = {"tag", "version", "cnt", "total"};
	result.output_schema.types = {
	    duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::BIGINT,
	    duckdb::LogicalType::DOUBLE,
	};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ24KernelInstance(const QueryParameters &parameters,
                                                                   const QueryKernelFactoryData *) {
	return CreateSecEdgarQ24FusedKernel(dynamic_cast<const SecEdgarQ24Parameters &>(parameters));
}

} // namespace

NativeScanSpec CreateSecEdgarQ24PreScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "pre";
	scan_spec.projected_columns = {
	    {"adsh", "adsh", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"tag", "tag", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"version", "version", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

NativeScanSpec CreateSecEdgarQ24NumScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "num";
	scan_spec.projected_columns = {
	    {"adsh", "adsh", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"tag", "tag", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"version", "version", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"value", "value", duckdb::LogicalType::DOUBLE, duckdb::PhysicalType::DOUBLE},
	    {"uom", "uom", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"ddate", "ddate", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	scan_spec.filters = {
	    {"uom", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value("USD")},
	    {"value", duckdb::ExpressionType::OPERATOR_IS_NOT_NULL, duckdb::Value()},
	    {"ddate", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO, duckdb::Value::INTEGER(20230101)},
	    {"ddate", duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO, duckdb::Value::INTEGER(20231231)},
	};
	return scan_spec;
}

NativeScanSpec CreateSecEdgarQ24NumFusedScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "num";
	scan_spec.projected_columns = {
	    {"adsh", "adsh", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"tag", "tag", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"version", "version", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"value", "value", duckdb::LogicalType::DOUBLE, duckdb::PhysicalType::DOUBLE},
	    {"uom", "uom", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"ddate", "ddate", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	// Table filters: uom/ddate pushed to storage; value null-check in kernel.
	scan_spec.filters = {
	    {"uom", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value("USD")},
	    {"ddate", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO, duckdb::Value::INTEGER(20230101)},
	    {"ddate", duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO, duckdb::Value::INTEGER(20231231)},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateSecEdgarQ24FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_sec_edgar_q24";
	package.overloads = {{}};
	package.bind_query = BindSecEdgarQ24Package;
	package.create_kernel = CreateSecEdgarQ24KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
