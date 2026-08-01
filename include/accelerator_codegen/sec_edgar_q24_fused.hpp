#pragma once

#include "accelerator_codegen/sec_edgar_q24.hpp"

#include "duckdb/main/client_context.hpp"

namespace duckdb {
class FunctionData;
}

namespace accelerator_codegen {

class SecEdgarQ24FusedKernel final : public SecEdgarQ24Kernel {
public:
	explicit SecEdgarQ24FusedKernel(const SecEdgarQ24Parameters &params, SecEdgarQ24ProfileCounters *profile = nullptr);

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override;

	void RunFusedPreScan(duckdb::ClientContext &context, const duckdb::FunctionData &num_bind);
};

void RunSecEdgarQ24FusedPreScan(KernelInstance &kernel, duckdb::ClientContext &context,
                                const duckdb::FunctionData &num_bind);

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ24FusedKernel(const SecEdgarQ24Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ24FusedProfilingKernel(const SecEdgarQ24Parameters &params,
                                                                         SecEdgarQ24ProfileCounters &counters);

} // namespace accelerator_codegen
