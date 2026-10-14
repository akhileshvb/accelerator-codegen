#include "backends/duckdb/api/query_runtime.hpp"
#include "sec_edgar/q1/duckdb/base/query.hpp"
#include "sec_edgar/q1/duckdb/base/fused.hpp"

#include <memory>
#include <stdexcept>

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

QueryBindResult BindSecEdgarQ1Package(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &) {
	SecEdgarQ1Parameters parameters;
	parameters.row_estimate = GetNativeScanTableRowCount(context, CreateSecEdgarQ1ScanSpec());

	QueryBindResult result;
	result.query_parameters = std::make_unique<SecEdgarQ1Parameters>(parameters);
	result.input_scans.push_back(CreateSecEdgarQ1ScanSpec());
	result.output_schema.names = {"stmt", "rfile", "cnt", "num_filings", "avg_line_num"};
	result.output_schema.types = {
	    duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::BIGINT,
	    duckdb::LogicalType::BIGINT,
	    duckdb::LogicalType::DOUBLE,
	};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ1KernelInstance(const QueryParameters &parameters,
                                                                  const QueryKernelFactoryData *) {
	return CreateSecEdgarQ1FusedKernel(dynamic_cast<const SecEdgarQ1Parameters &>(parameters));
}

} // namespace

NativeScanSpec CreateSecEdgarQ1ScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "pre";
	scan_spec.projected_columns = {
	    {"stmt", "stmt", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"rfile", "rfile", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"adsh", "adsh", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"line", "line", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	scan_spec.filters = {
	    {"stmt", duckdb::ExpressionType::OPERATOR_IS_NOT_NULL, duckdb::Value()},
	};
	return scan_spec;
}

namespace {

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ1FusedKernelInstance(const QueryParameters &parameters,
                                                                       const QueryKernelFactoryData *) {
	return CreateSecEdgarQ1FusedKernel(dynamic_cast<const SecEdgarQ1Parameters &>(parameters));
}

void RunSecEdgarQ1FusedScan(duckdb::ClientContext &context, KernelInstance &kernel,
                            const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) {
	if (bound_inputs.size() != 1) {
		throw std::runtime_error("SEC EDGAR fused scan expects exactly one bound input");
	}
	RunSecEdgarQ1FusedPreScan(kernel, context, *bound_inputs[0]);
}

} // namespace

duckdb::TableFunctionSet CreateSecEdgarQ1FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_sec_edgar_q1";
	package.overloads = {{}};
	package.bind_query = BindSecEdgarQ1Package;
	package.create_kernel = CreateSecEdgarQ1KernelInstance;
	package.create_fused_kernel = CreateSecEdgarQ1FusedKernelInstance;
	package.fused_scan = RunSecEdgarQ1FusedScan;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
