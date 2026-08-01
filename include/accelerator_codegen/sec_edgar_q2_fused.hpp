#pragma once

#include "accelerator_codegen/sec_edgar_q2.hpp"

#include "duckdb/main/client_context.hpp"

namespace duckdb {
class FunctionData;
}

namespace accelerator_codegen {

class SecEdgarQ2FusedKernel final : public SecEdgarQ2Kernel {
public:
	explicit SecEdgarQ2FusedKernel(const SecEdgarQ2Parameters &params, SecEdgarQ2ProfileCounters *profile = nullptr);

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override;

	void RunFusedPreScan(duckdb::ClientContext &context, const duckdb::FunctionData &num_bind);
};

void RunSecEdgarQ2FusedPreScan(KernelInstance &kernel, duckdb::ClientContext &context,
                               const duckdb::FunctionData &num_bind);

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ2FusedKernel(const SecEdgarQ2Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ2FusedProfilingKernel(const SecEdgarQ2Parameters &params,
                                                                          SecEdgarQ2ProfileCounters &counters);

} // namespace accelerator_codegen
