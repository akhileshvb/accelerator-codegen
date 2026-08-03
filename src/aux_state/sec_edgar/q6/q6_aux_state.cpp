#include "accelerator_codegen/sec_edgar_q6_aux.hpp"
#include "accelerator_codegen/sec_edgar_q6_rebuild_policy.hpp"
#include "accelerator_codegen/sec_edgar_q6_write_policy.hpp"

#include <chrono>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/sec_edgar_q6.hpp"
#include "accelerator_codegen/sec_edgar_q6_fused.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

SecEdgarQ6AuxDiagnostics g_sec_edgar_q6_aux_diagnostics_mut;

aux_state::PartitionedStateStore &GetSecEdgarQ6Store(duckdb::ClientContext &context) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	return registry->GetStore(kSecEdgarQ6AuxAcceleratorId);
}

struct BuildScratch {
	SecEdgarQ6Fy2023CohortPartition cohort;
	SecEdgarQ6PreIsBridgePartition pre_bridge;
	SecEdgarQ6TagVersionPartition tag_versions;
	SecEdgarQ6GroupAggregatesPartition aggregates;
	SecEdgarQ6OutputPartition output;
};

void BuildAllPartitions(duckdb::ClientContext &context, BuildScratch &scratch) {
	SecEdgarQ6Parameters parameters;
	parameters.row_estimate = GetNativeScanTableRowCount(context, CreateSecEdgarQ6NumScanSpec());
	auto kernel = CreateSecEdgarQ6FusedKernel(parameters);
	auto &fused_kernel = dynamic_cast<SecEdgarQ6FusedKernel &>(*kernel);
	auto num_bind = BindNativeScan(context, CreateSecEdgarQ6NumScanSpec());
	fused_kernel.RunFusedPreScan(context, *num_bind);
	kernel->Finalize();
	fused_kernel.ExportAuxPartitions(scratch.cohort, scratch.pre_bridge, scratch.tag_versions, scratch.aggregates,
	                               scratch.output);
}

void PublishPartitions(duckdb::ClientContext &context, const BuildScratch &scratch) {
	auto &store = GetSecEdgarQ6Store(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	const auto publish_epoch = registry->GetMaintenanceState(kSecEdgarQ6AuxAcceleratorId).BumpCommittedMutationEpoch();
	store.PublishPartition(kSecEdgarQ6Fy2023CohortSlot, std::make_shared<SecEdgarQ6Fy2023CohortPartition>(scratch.cohort),
	                       publish_epoch);
	store.PublishPartition(kSecEdgarQ6PreIsBridgeSlot, std::make_shared<SecEdgarQ6PreIsBridgePartition>(scratch.pre_bridge),
	                       publish_epoch);
	store.PublishPartition(kSecEdgarQ6TagVersionSlot, std::make_shared<SecEdgarQ6TagVersionPartition>(scratch.tag_versions),
	                       publish_epoch);
	store.PublishPartition(kSecEdgarQ6GroupAggregatesSlot,
	                       std::make_shared<SecEdgarQ6GroupAggregatesPartition>(scratch.aggregates), publish_epoch);
	store.PublishPartition(kSecEdgarQ6OutputSlot, std::make_shared<SecEdgarQ6OutputPartition>(scratch.output),
	                       publish_epoch);
}

} // namespace

void ResetSecEdgarQ6AuxDiagnostics() {
	g_sec_edgar_q6_aux_diagnostics_mut = SecEdgarQ6AuxDiagnostics {};
}

void UpdateSecEdgarQ6AuxDiagnostics(const SecEdgarQ6AuxDiagnostics &diagnostics) {
	g_sec_edgar_q6_aux_diagnostics_mut = diagnostics;
}

const SecEdgarQ6AuxDiagnostics &GetSecEdgarQ6AuxDiagnostics() {
	return g_sec_edgar_q6_aux_diagnostics_mut;
}

aux_state::ExecutionGateResult
CheckSecEdgarQ6GroupExecutionGate(const aux_state::PartitionedStateStore &store,
                                  const aux_state::TxnInvalidationBuffer &txn_buffer,
                                  uint64_t reader_visible_mutation_epoch) {
	aux_state::ExecutionGateResult result;
	if (txn_buffer.IsAllBypassed()) {
		result.bypassed_by_txn = true;
		result.committed_invalid = true;
		return result;
	}
	bool saw_valid = false;
	bool output_payload_ready = false;
	for (const auto slot : SecEdgarQ6FixedPartitionKeys()) {
		const auto status = store.GetStatus(slot);
		if (status != aux_state::PartitionStatus::Valid) {
			if (status == aux_state::PartitionStatus::Invalid) {
				result.committed_invalid = true;
			}
			continue;
		}
		saw_valid = true;
		if (txn_buffer.IsPartitionBypassed(slot)) {
			result.bypassed_by_txn = true;
		}
		const auto valid_epoch = store.GetValidThroughMutationEpoch(slot);
		if (valid_epoch > reader_visible_mutation_epoch) {
			result.committed_invalid = true;
		}
	}
	if (saw_valid) {
		auto payload = store.GetReadablePayload(kSecEdgarQ6OutputSlot);
		output_payload_ready = payload != nullptr;
	}
	if (!saw_valid || !output_payload_ready) {
		result.committed_invalid = true;
	}
	if (result.bypassed_by_txn || result.committed_invalid) {
		return result;
	}
	result.use_aux = true;
	return result;
}

void BuildSecEdgarQ6AuxState(duckdb::ClientContext &context) {
	const auto start = std::chrono::steady_clock::now();
	BuildScratch scratch;
	BuildAllPartitions(context, scratch);
	PublishPartitions(context, scratch);
	RecordSecEdgarQ6Rebuild();
	g_sec_edgar_q6_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void RebuildSecEdgarQ6AuxPartition(duckdb::ClientContext &context, SecEdgarQ6PartitionKey partition) {
	(void)partition;
	BuildSecEdgarQ6AuxState(context);
}

void InvalidateSecEdgarQ6AuxStateForTests(duckdb::ClientContext &context) {
	GetSecEdgarQ6Store(context).InvalidateAll();
}

std::shared_ptr<const SecEdgarQ6OutputPartition> GetSecEdgarQ6AuxOutputPartition(duckdb::ClientContext &context) {
	EnsurePendingSecEdgarQ6RebuildApplied(context);
	auto &store = GetSecEdgarQ6Store(context);
	auto payload = store.GetReadablePayload(kSecEdgarQ6OutputSlot);
	if (!payload) {
		return nullptr;
	}
	g_sec_edgar_q6_aux_diagnostics_mut.partitions_read = 1;
	return std::static_pointer_cast<const SecEdgarQ6OutputPartition>(payload);
}

} // namespace accelerator_codegen
