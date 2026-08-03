#include "accelerator_codegen/sec_edgar_q2_aux.hpp"
#include "accelerator_codegen/sec_edgar_q2_rebuild_policy.hpp"
#include "accelerator_codegen/sec_edgar_q2_write_policy.hpp"

#include <chrono>
#include <functional>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/chunk_source.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/sec_edgar_q2.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

SecEdgarQ2AuxDiagnostics g_sec_edgar_q2_aux_diagnostics_mut;

std::string ToStoredString(const duckdb::string_t &value) {
	const auto size = value.GetSize();
	if (size == 0) {
		return std::string();
	}
	const auto *data = value.GetData();
	if (data == nullptr) {
		return std::string(size, '\0');
	}
	return std::string(data, size);
}

aux_state::PartitionedStateStore &GetSecEdgarQ2Store(duckdb::ClientContext &context) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	return registry->GetStore(kSecEdgarQ2AuxAcceleratorId);
}

void ScanFy2022Cohort(duckdb::ClientContext &context, SecEdgarQ2Fy2022CohortPartition &cohort) {
	auto bind = BindNativeScan(context, CreateSecEdgarQ2SubScanSpec());
	auto source = CreateNativeChunkSource(context, *bind);
	while (source->Next()) {
		auto &chunk = source->Chunk();
		if (chunk.size() == 0) {
			continue;
		}
		auto &adsh_vec = chunk.data[SecEdgarQ2SubChunkLayout::ADSH_INDEX];
		auto &name_vec = chunk.data[SecEdgarQ2SubChunkLayout::NAME_INDEX];
		duckdb::UnifiedVectorFormat adsh_format;
		duckdb::UnifiedVectorFormat name_format;
		adsh_vec.ToUnifiedFormat(chunk.size(), adsh_format);
		name_vec.ToUnifiedFormat(chunk.size(), name_format);
		auto adsh_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(adsh_format);
		auto name_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto adsh_index = adsh_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			if (!adsh_format.validity.AllValid() && !adsh_format.validity.RowIsValidUnsafe(adsh_index)) {
				continue;
			}
			if (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index)) {
				continue;
			}
			cohort.adsh_to_name.emplace(SecEdgarQ2AdshKey::From(adsh_values[adsh_index]),
			                            ToStoredString(name_values[name_index]));
		}
	}
}

void ScanPureNumRows(duckdb::ClientContext &context,
                     const std::function<void(const duckdb::string_t &, const duckdb::string_t &, double)> &row_fn) {
	auto bind = BindNativeScan(context, CreateSecEdgarQ2NumScanSpec());
	auto source = CreateNativeChunkSource(context, *bind);
	while (source->Next()) {
		auto &chunk = source->Chunk();
		if (chunk.size() == 0) {
			continue;
		}
		auto &adsh_vec = chunk.data[SecEdgarQ2NumChunkLayout::ADSH_INDEX];
		auto &tag_vec = chunk.data[SecEdgarQ2NumChunkLayout::TAG_INDEX];
		auto &value_vec = chunk.data[SecEdgarQ2NumChunkLayout::VALUE_INDEX];
		duckdb::UnifiedVectorFormat adsh_format;
		duckdb::UnifiedVectorFormat tag_format;
		duckdb::UnifiedVectorFormat value_format;
		adsh_vec.ToUnifiedFormat(chunk.size(), adsh_format);
		tag_vec.ToUnifiedFormat(chunk.size(), tag_format);
		value_vec.ToUnifiedFormat(chunk.size(), value_format);
		auto adsh_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(adsh_format);
		auto tag_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(tag_format);
		auto value_values = duckdb::UnifiedVectorFormat::GetData<double>(value_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto adsh_index = adsh_format.sel->get_index(row);
			const auto tag_index = tag_format.sel->get_index(row);
			const auto value_index = value_format.sel->get_index(row);
			if (!value_format.validity.AllValid() && !value_format.validity.RowIsValidUnsafe(value_index)) {
				continue;
			}
			if (!adsh_format.validity.AllValid() && !adsh_format.validity.RowIsValidUnsafe(adsh_index)) {
				continue;
			}
			if (!tag_format.validity.AllValid() && !tag_format.validity.RowIsValidUnsafe(tag_index)) {
				continue;
			}
			row_fn(adsh_values[adsh_index], tag_values[tag_index], value_values[value_index]);
		}
	}
}

struct SecEdgarQ2BuildState {
	SecEdgarQ2MaxPureMapPartition max_map;
	SecEdgarQ2Fy2022CohortPartition cohort;
	std::vector<SecEdgarQ2CandidateRow> raw_candidates;
	SecEdgarQ2CandidatePartition candidates;
};

void BuildAllPartitions(duckdb::ClientContext &context, SecEdgarQ2BuildState &state) {
	ScanFy2022Cohort(context, state.cohort);

	ScanPureNumRows(context, [&](const duckdb::string_t &adsh, const duckdb::string_t &tag, double value) {
		const auto group_key = SecEdgarQ2GroupKey::From(adsh, tag);
		auto it = state.max_map.max_by_adsh_tag.find(group_key);
		if (it == state.max_map.max_by_adsh_tag.end()) {
			state.max_map.max_by_adsh_tag.emplace(group_key, value);
			return;
		}
		if (value > it->second) {
			it->second = value;
		}
	});

	ScanPureNumRows(context, [&](const duckdb::string_t &adsh, const duckdb::string_t &tag, double value) {
		const auto adsh_key = SecEdgarQ2AdshKey::From(adsh);
		const auto cohort_it = state.cohort.adsh_to_name.find(adsh_key);
		if (cohort_it == state.cohort.adsh_to_name.end()) {
			return;
		}
		const auto group_key = SecEdgarQ2GroupKey::From(adsh, tag);
		const auto max_it = state.max_map.max_by_adsh_tag.find(group_key);
		if (max_it == state.max_map.max_by_adsh_tag.end() || max_it->second != value) {
			return;
		}
		state.raw_candidates.push_back({cohort_it->second, ToStoredString(tag), value});
	});

	state.candidates.rows_at_max = std::move(state.raw_candidates);
	RefreshSecEdgarQ2CandidateOutput(state.candidates);
}

void PublishPartitions(duckdb::ClientContext &context, const SecEdgarQ2BuildState &state) {
	auto &store = GetSecEdgarQ2Store(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	const auto publish_epoch = registry->GetMaintenanceState(kSecEdgarQ2AuxAcceleratorId).BumpCommittedMutationEpoch();
	store.PublishPartition(kSecEdgarQ2MaxPureMapSlot,
	                       std::make_shared<SecEdgarQ2MaxPureMapPartition>(state.max_map), publish_epoch);
	store.PublishPartition(kSecEdgarQ2Fy2022CohortSlot,
	                       std::make_shared<SecEdgarQ2Fy2022CohortPartition>(state.cohort), publish_epoch);
	store.PublishPartition(kSecEdgarQ2MaxCandidatesSlot,
	                       std::make_shared<SecEdgarQ2CandidatePartition>(state.candidates), publish_epoch);
}

} // namespace

void ResetSecEdgarQ2AuxDiagnostics() {
	g_sec_edgar_q2_aux_diagnostics_mut = SecEdgarQ2AuxDiagnostics {};
}

void UpdateSecEdgarQ2AuxDiagnostics(const SecEdgarQ2AuxDiagnostics &diagnostics) {
	g_sec_edgar_q2_aux_diagnostics_mut = diagnostics;
}

const SecEdgarQ2AuxDiagnostics &GetSecEdgarQ2AuxDiagnostics() {
	return g_sec_edgar_q2_aux_diagnostics_mut;
}

aux_state::ExecutionGateResult
CheckSecEdgarQ2GroupExecutionGate(const aux_state::PartitionedStateStore &store,
                                  const aux_state::TxnInvalidationBuffer &txn_buffer,
                                  uint64_t reader_visible_mutation_epoch) {
	aux_state::ExecutionGateResult result;
	if (txn_buffer.IsAllBypassed()) {
		result.bypassed_by_txn = true;
		result.committed_invalid = true;
		return result;
	}
	bool saw_valid = false;
	for (const auto slot : SecEdgarQ2FixedPartitionKeys()) {
		const auto status = store.GetStatus(slot);
		if (status != aux_state::PartitionStatus::Valid) {
			if (status == aux_state::PartitionStatus::Invalid) {
				result.committed_invalid = true;
			}
			continue;
		}
		auto payload = store.GetReadablePayload(slot);
		if (!payload) {
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
	if (!saw_valid) {
		result.committed_invalid = true;
	}
	if (result.bypassed_by_txn || result.committed_invalid) {
		return result;
	}
	result.use_aux = true;
	return result;
}

void BuildSecEdgarQ2AuxState(duckdb::ClientContext &context) {
	const auto start = std::chrono::steady_clock::now();
	SecEdgarQ2BuildState state;
	BuildAllPartitions(context, state);
	PublishPartitions(context, state);
	RecordSecEdgarQ2Rebuild();
	g_sec_edgar_q2_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void RebuildSecEdgarQ2AuxPartition(duckdb::ClientContext &context, SecEdgarQ2PartitionKey partition) {
	const auto start = std::chrono::steady_clock::now();
	SecEdgarQ2BuildState state;
	BuildAllPartitions(context, state);
	auto &store = GetSecEdgarQ2Store(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	const auto publish_epoch = registry->GetMaintenanceState(kSecEdgarQ2AuxAcceleratorId).BumpCommittedMutationEpoch();
	switch (partition) {
	case kSecEdgarQ2MaxPureMapSlot:
		store.PublishPartition(partition, std::make_shared<SecEdgarQ2MaxPureMapPartition>(state.max_map), publish_epoch);
		break;
	case kSecEdgarQ2Fy2022CohortSlot:
		store.PublishPartition(partition, std::make_shared<SecEdgarQ2Fy2022CohortPartition>(state.cohort), publish_epoch);
		break;
	case kSecEdgarQ2MaxCandidatesSlot:
		store.PublishPartition(partition, std::make_shared<SecEdgarQ2CandidatePartition>(state.candidates), publish_epoch);
		break;
	default:
		BuildSecEdgarQ2AuxState(context);
		return;
	}
	RecordSecEdgarQ2Rebuild();
	g_sec_edgar_q2_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void InvalidateSecEdgarQ2AuxStateForTests(duckdb::ClientContext &context) {
	GetSecEdgarQ2Store(context).InvalidateAll();
}

std::vector<SecEdgarQ2OutputRow> CollectSecEdgarQ2AuxResult(duckdb::ClientContext &context) {
	EnsurePendingSecEdgarQ2RebuildApplied(context);
	auto &store = GetSecEdgarQ2Store(context);
	auto payload = store.GetReadablePayload(kSecEdgarQ2MaxCandidatesSlot);
	if (!payload) {
		return {};
	}
	const auto &partition = *std::static_pointer_cast<const SecEdgarQ2CandidatePartition>(payload);
	g_sec_edgar_q2_aux_diagnostics_mut.partitions_read = 3;
	return partition.output_rows;
}

} // namespace accelerator_codegen
