#include "accelerator_codegen/sec_edgar_q4_aux.hpp"
#include "accelerator_codegen/sec_edgar_q4_rebuild_policy.hpp"
#include "accelerator_codegen/sec_edgar_q4_write_policy.hpp"

#include <chrono>
#include <vector>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/sec_edgar_q4.hpp"
#include "accelerator_codegen/sec_edgar_q4_fused.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

SecEdgarQ4AuxDiagnostics g_sec_edgar_q4_aux_diagnostics_mut;

aux_state::PartitionedStateStore &GetSecEdgarQ4Store(duckdb::ClientContext &context) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	return registry->GetStore(kSecEdgarQ4AuxAcceleratorId);
}

struct BuildScratch {
	SecEdgarQ4SicCohortPartition cohort;
	SecEdgarQ4PreEqBridgePartition pre_bridge;
	SecEdgarQ4TagTlabelPartition tag_tlabel;
	SecEdgarQ4GroupAggregatesPartition aggregates;
	SecEdgarQ4OutputPartition output;
};

void BuildAllPartitions(duckdb::ClientContext &context, BuildScratch &scratch) {
	SecEdgarQ4Parameters parameters;
	parameters.row_estimate = GetNativeScanTableRowCount(context, CreateSecEdgarQ4NumScanSpec());
	auto kernel = CreateSecEdgarQ4FusedKernel(parameters);
	auto &fused_kernel = dynamic_cast<SecEdgarQ4FusedKernel &>(*kernel);
	auto num_bind = BindNativeScan(context, CreateSecEdgarQ4NumScanSpec());
	fused_kernel.RunFusedPreScan(context, *num_bind);
	kernel->Finalize();
	fused_kernel.ExportAuxPartitions(scratch.cohort, scratch.pre_bridge, scratch.tag_tlabel, scratch.aggregates,
	                               scratch.output);
}

void PublishPartitions(duckdb::ClientContext &context, const BuildScratch &scratch) {
	auto &store = GetSecEdgarQ4Store(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	const auto publish_epoch = registry->GetMaintenanceState(kSecEdgarQ4AuxAcceleratorId).BumpCommittedMutationEpoch();
	store.PublishPartition(kSecEdgarQ4SicCohortSlot, std::make_shared<SecEdgarQ4SicCohortPartition>(scratch.cohort),
	                       publish_epoch);
	store.PublishPartition(kSecEdgarQ4PreEqBridgeSlot, std::make_shared<SecEdgarQ4PreEqBridgePartition>(scratch.pre_bridge),
	                       publish_epoch);
	store.PublishPartition(kSecEdgarQ4TagTlabelSlot, std::make_shared<SecEdgarQ4TagTlabelPartition>(scratch.tag_tlabel),
	                       publish_epoch);
	store.PublishPartition(kSecEdgarQ4GroupAggregatesSlot,
	                       std::make_shared<SecEdgarQ4GroupAggregatesPartition>(scratch.aggregates), publish_epoch);
	store.PublishPartition(kSecEdgarQ4OutputSlot, std::make_shared<SecEdgarQ4OutputPartition>(scratch.output),
	                       publish_epoch);
}

} // namespace

void ResetSecEdgarQ4AuxDiagnostics() {
	g_sec_edgar_q4_aux_diagnostics_mut = SecEdgarQ4AuxDiagnostics {};
}

void UpdateSecEdgarQ4AuxDiagnostics(const SecEdgarQ4AuxDiagnostics &diagnostics) {
	g_sec_edgar_q4_aux_diagnostics_mut = diagnostics;
}

const SecEdgarQ4AuxDiagnostics &GetSecEdgarQ4AuxDiagnostics() {
	return g_sec_edgar_q4_aux_diagnostics_mut;
}

aux_state::ExecutionGateResult
CheckSecEdgarQ4GroupExecutionGate(const aux_state::PartitionedStateStore &store,
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
	for (const auto slot : SecEdgarQ4FixedPartitionKeys()) {
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
		auto payload = store.GetReadablePayload(kSecEdgarQ4OutputSlot);
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

void BuildSecEdgarQ4AuxState(duckdb::ClientContext &context) {
	const auto start = std::chrono::steady_clock::now();
	BuildScratch scratch;
	BuildAllPartitions(context, scratch);
	PublishPartitions(context, scratch);
	RecordSecEdgarQ4Rebuild();
	g_sec_edgar_q4_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void RebuildSecEdgarQ4AuxPartition(duckdb::ClientContext &context, SecEdgarQ4PartitionKey partition) {
	(void)partition;
	BuildSecEdgarQ4AuxState(context);
}

void InvalidateSecEdgarQ4AuxStateForTests(duckdb::ClientContext &context) {
	GetSecEdgarQ4Store(context).InvalidateAll();
}

std::shared_ptr<const SecEdgarQ4OutputPartition> GetSecEdgarQ4AuxOutputPartition(duckdb::ClientContext &context) {
	EnsurePendingSecEdgarQ4RebuildApplied(context);
	auto &store = GetSecEdgarQ4Store(context);
	auto payload = store.GetReadablePayload(kSecEdgarQ4OutputSlot);
	if (!payload) {
		return nullptr;
	}
	g_sec_edgar_q4_aux_diagnostics_mut.partitions_read = 1;
	return std::static_pointer_cast<const SecEdgarQ4OutputPartition>(payload);
}

} // namespace accelerator_codegen
