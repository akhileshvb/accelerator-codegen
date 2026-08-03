#include "accelerator_codegen/sec_edgar_q24_aux.hpp"
#include "accelerator_codegen/sec_edgar_q24_rebuild_policy.hpp"
#include "accelerator_codegen/sec_edgar_q24_write_policy.hpp"

#include <chrono>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/sec_edgar_q24.hpp"
#include "accelerator_codegen/sec_edgar_q24_fused.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

SecEdgarQ24AuxDiagnostics g_sec_edgar_q24_aux_diagnostics_mut;

aux_state::PartitionedStateStore &GetSecEdgarQ24Store(duckdb::ClientContext &context) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	return registry->GetStore(kSecEdgarQ24AuxAcceleratorId);
}

struct BuildScratch {
	SecEdgarQ24PreMembershipPartition pre_membership;
	SecEdgarQ24TagVersionPartition tag_versions;
	SecEdgarQ24GroupAggregatesPartition aggregates;
	SecEdgarQ24OutputPartition output;
};

void BuildAllPartitions(duckdb::ClientContext &context, BuildScratch &scratch) {
	SecEdgarQ24Parameters parameters;
	parameters.row_estimate = GetNativeScanTableRowCount(context, CreateSecEdgarQ24NumScanSpec());
	auto kernel = CreateSecEdgarQ24FusedKernel(parameters);
	auto &fused_kernel = dynamic_cast<SecEdgarQ24FusedKernel &>(*kernel);
	auto num_bind = BindNativeScan(context, CreateSecEdgarQ24NumScanSpec());
	fused_kernel.RunFusedPreScan(context, *num_bind);
	kernel->Finalize();
	fused_kernel.ExportAuxPartitions(scratch.pre_membership, scratch.tag_versions, scratch.aggregates, scratch.output);
}

void PublishPartitions(duckdb::ClientContext &context, const BuildScratch &scratch) {
	auto &store = GetSecEdgarQ24Store(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	const auto publish_epoch = registry->GetMaintenanceState(kSecEdgarQ24AuxAcceleratorId).BumpCommittedMutationEpoch();
	store.PublishPartition(kSecEdgarQ24PreMembershipSlot,
	                       std::make_shared<SecEdgarQ24PreMembershipPartition>(scratch.pre_membership), publish_epoch);
	store.PublishPartition(kSecEdgarQ24TagVersionSlot, std::make_shared<SecEdgarQ24TagVersionPartition>(scratch.tag_versions),
	                       publish_epoch);
	store.PublishPartition(kSecEdgarQ24GroupAggregatesSlot,
	                       std::make_shared<SecEdgarQ24GroupAggregatesPartition>(scratch.aggregates), publish_epoch);
	store.PublishPartition(kSecEdgarQ24OutputSlot, std::make_shared<SecEdgarQ24OutputPartition>(scratch.output),
	                       publish_epoch);
}

} // namespace

void ResetSecEdgarQ24AuxDiagnostics() {
	g_sec_edgar_q24_aux_diagnostics_mut = SecEdgarQ24AuxDiagnostics {};
}

void UpdateSecEdgarQ24AuxDiagnostics(const SecEdgarQ24AuxDiagnostics &diagnostics) {
	g_sec_edgar_q24_aux_diagnostics_mut = diagnostics;
}

const SecEdgarQ24AuxDiagnostics &GetSecEdgarQ24AuxDiagnostics() {
	return g_sec_edgar_q24_aux_diagnostics_mut;
}

aux_state::ExecutionGateResult
CheckSecEdgarQ24GroupExecutionGate(const aux_state::PartitionedStateStore &store,
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
	for (const auto slot : SecEdgarQ24FixedPartitionKeys()) {
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
		auto payload = store.GetReadablePayload(kSecEdgarQ24OutputSlot);
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

void BuildSecEdgarQ24AuxState(duckdb::ClientContext &context) {
	const auto start = std::chrono::steady_clock::now();
	BuildScratch scratch;
	BuildAllPartitions(context, scratch);
	PublishPartitions(context, scratch);
	RecordSecEdgarQ24Rebuild();
	g_sec_edgar_q24_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void RebuildSecEdgarQ24AuxPartition(duckdb::ClientContext &context, SecEdgarQ24PartitionKey partition) {
	(void)partition;
	BuildSecEdgarQ24AuxState(context);
}

void InvalidateSecEdgarQ24AuxStateForTests(duckdb::ClientContext &context) {
	GetSecEdgarQ24Store(context).InvalidateAll();
}

std::shared_ptr<const SecEdgarQ24OutputPartition> GetSecEdgarQ24AuxOutputPartition(duckdb::ClientContext &context) {
	EnsurePendingSecEdgarQ24RebuildApplied(context);
	auto &store = GetSecEdgarQ24Store(context);
	auto payload = store.GetReadablePayload(kSecEdgarQ24OutputSlot);
	if (!payload) {
		return nullptr;
	}
	g_sec_edgar_q24_aux_diagnostics_mut.partitions_read = 1;
	return std::static_pointer_cast<const SecEdgarQ24OutputPartition>(payload);
}

} // namespace accelerator_codegen
