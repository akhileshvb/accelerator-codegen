#include "{{benchmark_name}}/q{{query_number}}/{{engine_name}}/base/query.hpp"

// Kernel scaffold for {{engine_name}}: fill Consume / Finalize / Produce with this
// engine's native batch or row types (backends/{{engine_name}}/api/README.md).

namespace accelerator_codegen {
namespace {

class {{type_prefix}}Kernel {
public:
	explicit {{type_prefix}}Kernel({{type_prefix}}Parameters params_p, {{type_prefix}}ProfileCounters *profile_p)
	    : params(std::move(params_p)), profile(profile_p) {
	}

	// TODO({{engine_name}}): the host's consume entrypoint over its native batch or row range.
	void Consume(/* native input */) {
		if (profile) {
			profile->generic_chunks++;
		}
		consumed = true;
	}

	void Finalize() {
		finalized = true;
	}

	// TODO({{engine_name}}): produce into the host's output representation.
	bool Produce(/* native output */) {
		return false;
	}

private:
	{{type_prefix}}Parameters params;
	{{type_prefix}}ProfileCounters *profile;
	bool consumed = false;
	bool finalized = false;
};

} // namespace

std::unique_ptr<{{type_prefix}}Kernel> {{kernel_fn}}(const {{type_prefix}}Parameters &params) {
	return std::make_unique<{{type_prefix}}Kernel>(params, nullptr);
}

std::unique_ptr<{{type_prefix}}Kernel> {{profiling_kernel_fn}}(const {{type_prefix}}Parameters &params,
                                                              {{type_prefix}}ProfileCounters &counters) {
	return std::make_unique<{{type_prefix}}Kernel>(params, &counters);
}

} // namespace accelerator_codegen
