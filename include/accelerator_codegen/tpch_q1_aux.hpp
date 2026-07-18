#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/tpch_q1.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {

//! ACCEL_Q1_AUX_STATE=1 enables the immutable shipdate-prefix PoC (default off).
bool Q1AuxStateEnabled();

struct Q1AuxStateDiagnostics {
	bool used_aux_path = false;
	bool built_this_call = false;
	bool refused_stale = false;
	bool refused_local_writes = false;
	duckdb::idx_t date_entries = 0;
	duckdb::idx_t group_entries = 0;
	duckdb::idx_t prefix_cells = 0;
	duckdb::idx_t source_rows = 0;
	duckdb::idx_t memory_bytes = 0;
	duckdb::idx_t build_ns = 0;
	duckdb::idx_t lookup_ns = 0;
	duckdb::idx_t finalize_ns = 0;
};

//! Drop any cached prefix state (tests / rebuild).
void InvalidateQ1AuxState();

//! Build (or rebuild) the immutable shipdate-prefix snapshot from native lineitem storage.
//! Throws if the transaction has uncommitted local modifications on lineitem.
void BuildQ1AuxState(duckdb::ClientContext &context);

const Q1AuxStateDiagnostics &GetQ1AuxStateDiagnostics();

//! Kernel that answers Q1 from the process-local prefix snapshot (no scan on query path).
class Q1AuxKernel final : public KernelInstance {
public:
	explicit Q1AuxKernel(Q1Parameters params);
	~Q1AuxKernel() override;

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override;
	void Finalize() override;
	bool ProduceOutput(duckdb::DataChunk &output) override;

	//! Ensure snapshot is valid for this context, then materialize groups for cutoff_date.
	void AnswerFromAuxState(duckdb::ClientContext &context);

	const Q1AuxStateDiagnostics &Diagnostics() const {
		return diagnostics;
	}

private:
	Q1Parameters params;
	Q1AuxStateDiagnostics diagnostics;
	struct State;
	std::unique_ptr<State> state;
};

duckdb::unique_ptr<KernelInstance> CreateQ1AuxKernel(const Q1Parameters &params);

//! Unfiltered lineitem projection used only to construct the aux snapshot.
NativeScanSpec CreateQ1AuxBuildScanSpec();

} // namespace accelerator_codegen
