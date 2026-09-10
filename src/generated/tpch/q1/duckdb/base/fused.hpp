#pragma once

#include <cstdint>
#include <memory>

#include "accelerator_codegen/runtime/duckdb/kernel.hpp"
#include "tpch/q1/duckdb/base/query.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {
class FunctionData;
}

namespace accelerator_codegen {

enum class Q1FusedAggMode : uint8_t {
	//! Prefer dominant dictionary / fixed-slot; fall back to scalar UnifiedVectorFormat.
	FixedSlotPreferred = 0,
	//! Force scalar UnifiedVectorFormat aggregation only.
	ScalarOnly = 1,
};

struct Q1FusedLayoutCounters {
	duckdb::idx_t vectors_inspected = 0;
	duckdb::idx_t qty_dictionary = 0;
	duckdb::idx_t qty_flat = 0;
	duckdb::idx_t qty_other = 0;
	duckdb::idx_t ep_dictionary = 0;
	duckdb::idx_t ep_flat = 0;
	duckdb::idx_t disc_dictionary = 0;
	duckdb::idx_t disc_flat = 0;
	duckdb::idx_t tax_dictionary = 0;
	duckdb::idx_t tax_flat = 0;
	duckdb::idx_t rf_dictionary = 0;
	duckdb::idx_t rf_flat = 0;
	duckdb::idx_t rf_other = 0;
	duckdb::idx_t ls_dictionary = 0;
	duckdb::idx_t ls_flat = 0;
	duckdb::idx_t ls_other = 0;
	duckdb::idx_t numeric_shared_sel_ptr = 0;
	duckdb::idx_t numeric_shared_sel_values = 0;
	duckdb::idx_t numeric_divergent_sel = 0;
	duckdb::idx_t qty_child_all_valid = 0;
	duckdb::idx_t rf_child_all_valid = 0;
	duckdb::idx_t ls_child_all_valid = 0;
	duckdb::idx_t dominant_eligible = 0;
	duckdb::idx_t flat_eligible = 0;
};

struct Q1FusedScanDiagnostics {
	duckdb::idx_t vectors_consumed = 0;
	duckdb::idx_t qualifying_rows = 0;
	duckdb::idx_t residual_checks = 0;
	duckdb::idx_t chunks_materialized = 0;
	//! Wall time for payload Scan (aligned) or Select (Slice) materialization.
	duckdb::idx_t payload_decode_ns = 0;
	//! Alias of payload_decode_ns for older breakdown CSV readers.
	duckdb::idx_t payload_select_ns = 0;
	duckdb::idx_t aggregate_ns = 0;
	//! Aligned fixed-slot sub-timers (selection / index handling experiments).
	duckdb::idx_t sel_compose_ns = 0;
	duckdb::idx_t sel_index_ns = 0;
	duckdb::idx_t agg_update_ns = 0;
	duckdb::idx_t fixed_slot_vectors = 0;
	duckdb::idx_t fixed_slot_rows = 0;
	duckdb::idx_t flat_fast_vectors = 0;
	duckdb::idx_t flat_fast_rows = 0;
	duckdb::idx_t scalar_fallback_vectors = 0;
	duckdb::idx_t scalar_fallback_rows = 0;
	duckdb::idx_t aligned_payload_vectors = 0;
	duckdb::idx_t aligned_payload_rows = 0;
	duckdb::idx_t aligned_fixed_slot_vectors = 0;
	duckdb::idx_t aligned_fixed_slot_rows = 0;
	duckdb::idx_t aligned_identity_filter_vectors = 0;
	duckdb::idx_t aligned_nested_index_vectors = 0;
	duckdb::idx_t aligned_compose_sel_vectors = 0;
	bool used_fused_path = false;
	bool used_aligned_payload = false;
	bool refused_missing_filters = false;
	Q1FusedAggMode agg_mode = Q1FusedAggMode::FixedSlotPreferred;
	Q1FusedLayoutCounters layouts;
};

//! Production Q1 kernel: fused ScanFilteredConsumer path only (`q1_fused_scan.cpp`).
//! The older chunk DataChunk kernel (`q1_kernel.cpp`) was removed.
//! Ablation toggles (aligned/direct-index/scalar) require -DACCEL_EXPERIMENTAL_Q1_OPTS.
bool Q1FusedScanEnabled();

//! True when aligned payload scan is enabled (default on with fused).
//! Scan six payload columns without Slice, then aggregate on filter sel + decoded blocks.
bool Q1FusedAlignedPayloadEnabled();

//! When aligned: direct nested indexing (default on) indexes filter∘dict in the hot loop
//! without materializing composed SelectionVectors. COMPOSE_SEL forces compose (experimental).
bool Q1FusedDirectIndexEnabled();

//! Aggregation mode: fixed-slot preferred in production; SCALAR/COMPOSE via experimental opts.
Q1FusedAggMode GetQ1FusedAggMode();

//! KernelInstance that aggregates via DataTable::ScanFilteredConsumer (no DataChunk handoff).
//! Call RunFusedScan once before Finalize/ProduceOutput. ConsumeInput is unused.
class Q1FusedKernel final : public KernelInstance {
public:
	explicit Q1FusedKernel(Q1Parameters params);
	~Q1FusedKernel() override;

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override;
	void Finalize() override;
	bool ProduceOutput(duckdb::DataChunk &output) override;

	//! Drain the bound native scan with shipdate filtered once; aggregate with TrustFilter.
	//! Throws if filters are missing (refuses unsafe TrustFilter).
	void RunFusedScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data);

	const Q1FusedScanDiagnostics &Diagnostics() const {
		return diagnostics;
	}

private:
	Q1Parameters params;
	Q1FusedScanDiagnostics diagnostics;
	struct State;
	std::unique_ptr<State> state;
};

duckdb::unique_ptr<KernelInstance> CreateQ1FusedKernel(const Q1Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ1ParallelKernel(const Q1Parameters &params);

} // namespace accelerator_codegen
