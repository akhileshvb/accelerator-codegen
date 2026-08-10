#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "sec_edgar/q3/base/query.hpp"
#include "sec_edgar/q3/base/fused.hpp"

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

QueryBindResult BindSecEdgarQ3Package(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &) {
	SecEdgarQ3Parameters parameters;
	parameters.row_estimate = GetNativeScanTableRowCount(context, CreateSecEdgarQ3NumScanSpec());

	QueryBindResult result;
	result.query_parameters = std::make_unique<SecEdgarQ3Parameters>(parameters);
	result.input_scans.push_back(CreateSecEdgarQ3NumScanSpec());
	result.output_schema.names = {"name", "cik", "total_value"};
	result.output_schema.types = {
	    duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::INTEGER,
	    duckdb::LogicalType::DOUBLE,
	};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ3KernelInstance(const QueryParameters &parameters,
                                                                  const QueryKernelFactoryData *) {
	return CreateSecEdgarQ3FusedKernel(dynamic_cast<const SecEdgarQ3Parameters &>(parameters));
}

} // namespace

NativeScanSpec CreateSecEdgarQ3SubScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "sub";
	scan_spec.projected_columns = {
	    {"adsh", "adsh", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"cik", "cik", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	    {"fy", "fy", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	scan_spec.filters = {
	    {"fy", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value::INTEGER(2022)},
	};
	return scan_spec;
}

NativeScanSpec CreateSecEdgarQ3NumScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "num";
	scan_spec.projected_columns = {
	    {"adsh", "adsh", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"value", "value", duckdb::LogicalType::DOUBLE, duckdb::PhysicalType::DOUBLE},
	    {"uom", "uom", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	scan_spec.filters = {
	    {"value", duckdb::ExpressionType::OPERATOR_IS_NOT_NULL, duckdb::Value()},
	    {"uom", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value("USD")},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateSecEdgarQ3FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_sec_edgar_q3";
	package.overloads = {{}};
	package.bind_query = BindSecEdgarQ3Package;
	package.create_kernel = CreateSecEdgarQ3KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
