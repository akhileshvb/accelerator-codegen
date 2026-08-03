#include "accelerator_codegen/sec_edgar_q3_aux.hpp"
#include "accelerator_codegen/sec_edgar_q3_rebuild_policy.hpp"
#include "accelerator_codegen/sec_edgar_q3_write_policy.hpp"

#include <chrono>
#include <vector>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/sec_edgar_q3.hpp"
#include "accelerator_codegen/sec_edgar_q3_fused.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

SecEdgarQ3AuxDiagnostics g_sec_edgar_q3_aux_diagnostics_mut;

aux_state::PartitionedStateStore &GetSecEdgarQ3Store(duckdb::ClientContext &context) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	return registry->GetStore(kSecEdgarQ3AuxAcceleratorId);
}

struct BuildScratch {
	SecEdgarQ3Fy2022CohortPartition cohort;
	SecEdgarQ3CikTotalsPartition cik_totals;
	SecEdgarQ3OutputPartition output;
};

void BuildAllPartitions(duckdb::ClientContext &context, BuildScratch &scratch) {
	SecEdgarQ3Parameters parameters;
	parameters.row_estimate = GetNativeScanTableRowCount(context, CreateSecEdgarQ3NumScanSpec());
	auto kernel = CreateSecEdgarQ3FusedKernel(parameters);
	auto &fused_kernel = dynamic_cast<SecEdgarQ3FusedKernel &>(*kernel);
	auto num_bind = BindNativeScan(context, CreateSecEdgarQ3NumScanSpec());
	fused_kernel.RunFusedPreScan(context, *num_bind);
	kernel->Finalize();
	fused_kernel.ExportAuxPartitions(scratch.cohort, scratch.cik_totals, scratch.output);
}

void PublishPartitions(duckdb::ClientContext &context, const BuildScratch &scratch) {
	auto &store = GetSecEdgarQ3Store(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	const auto publish_epoch = registry->GetMaintenanceState(kSecEdgarQ3AuxAcceleratorId).BumpCommittedMutationEpoch();
	store.PublishPartition(kSecEdgarQ3Fy2022CohortSlot,
	                       std::make_shared<SecEdgarQ3Fy2022CohortPartition>(scratch.cohort), publish_epoch);
	store.PublishPartition(kSecEdgarQ3CikTotalsSlot,
	                       std::make_shared<SecEdgarQ3CikTotalsPartition>(scratch.cik_totals), publish_epoch);
	store.PublishPartition(kSecEdgarQ3OutputSlot, std::make_shared<SecEdgarQ3OutputPartition>(scratch.output),
	                       publish_epoch);
}

} // namespace

void ResetSecEdgarQ3AuxDiagnostics() {
	g_sec_edgar_q3_aux_diagnostics_mut = SecEdgarQ3AuxDiagnostics {};
}

void UpdateSecEdgarQ3AuxDiagnostics(const SecEdgarQ3AuxDiagnostics &diagnostics) {
	g_sec_edgar_q3_aux_diagnostics_mut = diagnostics;
}

const SecEdgarQ3AuxDiagnostics &GetSecEdgarQ3AuxDiagnostics() {
	return g_sec_edgar_q3_aux_diagnostics_mut;
}

aux_state::ExecutionGateResult
CheckSecEdgarQ3GroupExecutionGate(const aux_state::PartitionedStateStore &store,
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
	for (const auto slot : SecEdgarQ3FixedPartitionKeys()) {
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
		auto payload = store.GetReadablePayload(kSecEdgarQ3OutputSlot);
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

void BuildSecEdgarQ3AuxState(duckdb::ClientContext &context) {
	const auto start = std::chrono::steady_clock::now();
	BuildScratch scratch;
	BuildAllPartitions(context, scratch);
	PublishPartitions(context, scratch);
	RecordSecEdgarQ3Rebuild();
	g_sec_edgar_q3_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void RebuildSecEdgarQ3AuxPartition(duckdb::ClientContext &context, SecEdgarQ3PartitionKey partition) {
	const auto start = std::chrono::steady_clock::now();
	BuildScratch scratch;
	BuildAllPartitions(context, scratch);
	auto &store = GetSecEdgarQ3Store(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	const auto publish_epoch = registry->GetMaintenanceState(kSecEdgarQ3AuxAcceleratorId).BumpCommittedMutationEpoch();
	switch (partition) {
	case kSecEdgarQ3Fy2022CohortSlot:
		store.PublishPartition(partition, std::make_shared<SecEdgarQ3Fy2022CohortPartition>(scratch.cohort), publish_epoch);
		break;
	case kSecEdgarQ3CikTotalsSlot:
		store.PublishPartition(partition, std::make_shared<SecEdgarQ3CikTotalsPartition>(scratch.cik_totals), publish_epoch);
		break;
	case kSecEdgarQ3OutputSlot:
		store.PublishPartition(partition, std::make_shared<SecEdgarQ3OutputPartition>(scratch.output), publish_epoch);
		break;
	default:
		BuildSecEdgarQ3AuxState(context);
		return;
	}
	RecordSecEdgarQ3Rebuild();
	g_sec_edgar_q3_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void InvalidateSecEdgarQ3AuxStateForTests(duckdb::ClientContext &context) {
	GetSecEdgarQ3Store(context).InvalidateAll();
}

std::shared_ptr<const SecEdgarQ3OutputPartition> GetSecEdgarQ3AuxOutputPartition(duckdb::ClientContext &context) {
	EnsurePendingSecEdgarQ3RebuildApplied(context);
	auto &store = GetSecEdgarQ3Store(context);
	auto payload = store.GetReadablePayload(kSecEdgarQ3OutputSlot);
	if (!payload) {
		return nullptr;
	}
	g_sec_edgar_q3_aux_diagnostics_mut.partitions_read = 1;
	return std::static_pointer_cast<const SecEdgarQ3OutputPartition>(payload);
}

} // namespace accelerator_codegen
