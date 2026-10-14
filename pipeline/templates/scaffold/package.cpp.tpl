#include "{{benchmark_name}}/q{{query_number}}/{{engine_name}}/base/query.hpp"
#include "backends/duckdb/api/query_runtime.hpp"

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

QueryBindResult Bind{{type_prefix}}Package(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &) {
	{{type_prefix}}Parameters parameters;
	parameters.row_estimate = GetNativeScanTableRowCount(context, {{scan_spec_fn}}());

	QueryBindResult result;
	result.query_parameters = std::make_unique<{{type_prefix}}Parameters>(parameters);
	result.input_scans.push_back({{scan_spec_fn}}());
	result.output_schema.names = {{{output_schema_names}}};
	result.output_schema.types = {
	    {{output_schema_types}},
	};
	return result;
}

duckdb::unique_ptr<KernelInstance> Create{{type_prefix}}KernelInstance(const QueryParameters &parameters,
                                                                       const QueryKernelFactoryData *) {
	return {{kernel_fn}}(dynamic_cast<const {{type_prefix}}Parameters &>(parameters));
}

} // namespace

duckdb::TableFunctionSet {{create_fn}}() {
	QueryPackage package;
	package.function_name = "{{function_name}}";
	package.overloads = {};
	package.bind_query = Bind{{type_prefix}}Package;
	package.create_kernel = Create{{type_prefix}}KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
