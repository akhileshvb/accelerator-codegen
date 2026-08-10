#pragma once

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/parallel/parallel_runtime.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {

//! Parallel query kernel: drains scans itself via TaskExecutor; ConsumeInput unused.
class ParallelKernelInstance : public KernelInstance {
public:
	void ConsumeInput(duckdb::idx_t, duckdb::DataChunk &) override {
		throw duckdb::InternalException("ParallelKernelInstance::ConsumeInput is unused");
	}

	//! Execute all scan/build/probe phases. Called once from the table-function exec path.
	virtual void RunParallel(duckdb::ClientContext &context,
	                         const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) = 0;
};

} // namespace accelerator_codegen
