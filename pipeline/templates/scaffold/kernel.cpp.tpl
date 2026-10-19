#include "{{benchmark_name}}/q{{query_number}}/{{engine_name}}/base/query.hpp"

// Engine-parameterized kernel scaffold for host `{{engine_name}}`.
// Fill Consume / Finalize / Produce using THIS engine's native batch or row
// types (from the engine_backend engine profile or backends/{{engine_name}}/engine/).
// Do NOT hard-code duckdb::DataChunk unless engine_name is duckdb and you
// intentionally use backends/duckdb/engine/native_scan.hpp helpers.

namespace accelerator_codegen {
namespace {

class {{type_prefix}}Kernel final : public KernelInstance {
public:
	explicit {{type_prefix}}Kernel({{type_prefix}}Parameters params_p, {{type_prefix}}ProfileCounters *profile_p)
	    : params(std::move(params_p)), profile(profile_p) {
	}

	// TODO({{engine_name}}): override / implement the host's consume entrypoint
	// (e.g. slots, tuples, or duckdb DataChunk only when engine_name=duckdb).
	void ConsumeNativeInput(/* {{engine_name}} native batch or row range */) {
		if (profile) {
			profile->generic_chunks++;
		}
		consumed = true;
	}

	void Finalize() override {
		finalized = true;
	}

	// TODO({{engine_name}}): produce into the host's output representation.
	bool ProduceNativeOutput(/* {{engine_name}} output batch */) {
		return false;
	}

private:
	{{type_prefix}}Parameters params;
	{{type_prefix}}ProfileCounters *profile;
	bool consumed = false;
	bool finalized = false;
};

} // namespace

// Optional host-specific scan setup — not a portable DataChunk IR.
void {{scan_spec_fn}}(/* host scan args */) {
}

std::unique_ptr<KernelInstance> {{kernel_fn}}(const {{type_prefix}}Parameters &params) {
	return std::make_unique<{{type_prefix}}Kernel>(params, nullptr);
}

std::unique_ptr<KernelInstance> {{profiling_kernel_fn}}(const {{type_prefix}}Parameters &params,
                                                       {{type_prefix}}ProfileCounters &counters) {
	return std::make_unique<{{type_prefix}}Kernel>(params, &counters);
}

} // namespace accelerator_codegen
