#include "accelerator_codegen/{{header_basename}}.hpp"

#include "duckdb/common/exception.hpp"

namespace accelerator_codegen {
namespace {

class {{type_prefix}}Kernel final : public KernelInstance {
public:
	explicit {{type_prefix}}Kernel({{type_prefix}}Parameters params_p, {{type_prefix}}ProfileCounters *profile_p)
	    : params(std::move(params_p)), profile(profile_p) {
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		(void)input_id;
		(void)input;
		if (profile) {
			profile->generic_chunks++;
		}
		consumed = true;
	}

	void Finalize() override {
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		(void)output;
		return false;
	}

private:
	{{type_prefix}}Parameters params;
	{{type_prefix}}ProfileCounters *profile;
	bool consumed = false;
	bool finalized = false;
};

} // namespace

NativeScanSpec {{scan_spec_fn}}() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "pre";
	return scan_spec;
}

duckdb::unique_ptr<KernelInstance> {{kernel_fn}}(const {{type_prefix}}Parameters &params) {
	return duckdb::make_uniq<{{type_prefix}}Kernel>(params, nullptr);
}

duckdb::unique_ptr<KernelInstance> {{profiling_kernel_fn}}(const {{type_prefix}}Parameters &params,
                                                           {{type_prefix}}ProfileCounters &counters) {
	return duckdb::make_uniq<{{type_prefix}}Kernel>(params, &counters);
}

} // namespace accelerator_codegen
