#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "sec_edgar/q2/base/query.hpp"
#include "sec_edgar/q2/base/fused.hpp"

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

QueryBindResult BindSecEdgarQ2Package(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &) {
	SecEdgarQ2Parameters parameters;
	parameters.row_estimate = GetNativeScanTableRowCount(context, CreateSecEdgarQ2NumScanSpec());

	QueryBindResult result;
	result.query_parameters = std::make_unique<SecEdgarQ2Parameters>(parameters);
	result.input_scans.push_back(CreateSecEdgarQ2NumScanSpec());
	result.output_schema.names = {"name", "tag", "value"};
	result.output_schema.types = {
	    duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::DOUBLE,
	};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ2KernelInstance(const QueryParameters &parameters,
                                                                  const QueryKernelFactoryData *) {
	return CreateSecEdgarQ2FusedKernel(dynamic_cast<const SecEdgarQ2Parameters &>(parameters));
}

} // namespace

NativeScanSpec CreateSecEdgarQ2SubScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "sub";
	scan_spec.projected_columns = {
	    {"adsh", "adsh", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	scan_spec.filters = {
	    {"fy", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value::INTEGER(2022)},
	};
	return scan_spec;
}

NativeScanSpec CreateSecEdgarQ2NumScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "num";
	scan_spec.projected_columns = {
	    {"adsh", "adsh", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"tag", "tag", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"value", "value", duckdb::LogicalType::DOUBLE, duckdb::PhysicalType::DOUBLE},
	};
	scan_spec.filters = {
	    {"uom", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value("pure")},
	    {"value", duckdb::ExpressionType::OPERATOR_IS_NOT_NULL, duckdb::Value()},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateSecEdgarQ2FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_sec_edgar_q2";
	package.overloads = {{}};
	package.bind_query = BindSecEdgarQ2Package;
	package.create_kernel = CreateSecEdgarQ2KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
