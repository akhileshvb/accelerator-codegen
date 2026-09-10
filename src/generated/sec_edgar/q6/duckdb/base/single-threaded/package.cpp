#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "sec_edgar/q6/duckdb/base/query.hpp"
#include "sec_edgar/q6/duckdb/base/fused.hpp"

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

QueryBindResult BindSecEdgarQ6Package(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &) {
	SecEdgarQ6Parameters parameters;
	parameters.row_estimate = GetNativeScanTableRowCount(context, CreateSecEdgarQ6NumScanSpec());

	QueryBindResult result;
	result.query_parameters = std::make_unique<SecEdgarQ6Parameters>(parameters);
	result.input_scans.push_back(CreateSecEdgarQ6NumScanSpec());
	result.output_schema.names = {"name", "stmt", "tag", "plabel", "total_value", "cnt"};
	result.output_schema.types = {
	    duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::DOUBLE,
	    duckdb::LogicalType::BIGINT,
	};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ6KernelInstance(const QueryParameters &parameters,
                                                                  const QueryKernelFactoryData *) {
	return CreateSecEdgarQ6FusedKernel(dynamic_cast<const SecEdgarQ6Parameters &>(parameters));
}

} // namespace

NativeScanSpec CreateSecEdgarQ6SubScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "sub";
	scan_spec.projected_columns = {
	    {"adsh", "adsh", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"fy", "fy", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	scan_spec.filters = {
	    {"fy", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value::INTEGER(2023)},
	};
	return scan_spec;
}

NativeScanSpec CreateSecEdgarQ6PreScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "pre";
	scan_spec.projected_columns = {
	    {"adsh", "adsh", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"tag", "tag", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"version", "version", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"stmt", "stmt", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"plabel", "plabel", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	scan_spec.filters = {
	    {"stmt", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value("IS")},
	};
	return scan_spec;
}

NativeScanSpec CreateSecEdgarQ6NumScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "num";
	scan_spec.projected_columns = {
	    {"adsh", "adsh", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"tag", "tag", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"version", "version", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"value", "value", duckdb::LogicalType::DOUBLE, duckdb::PhysicalType::DOUBLE},
	    {"uom", "uom", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	scan_spec.filters = {
	    {"value", duckdb::ExpressionType::OPERATOR_IS_NOT_NULL, duckdb::Value()},
	    {"uom", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value("USD")},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateSecEdgarQ6FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_sec_edgar_q6";
	package.overloads = {{}};
	package.bind_query = BindSecEdgarQ6Package;
	package.create_kernel = CreateSecEdgarQ6KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
