#include "accelerator_codegen/sec_edgar_q1_aux.hpp"
#include "accelerator_codegen/sec_edgar_q1_rebuild_policy.hpp"
#include "accelerator_codegen/sec_edgar_q1_write_policy.hpp"

#include <chrono>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/chunk_source.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/sec_edgar_q1.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

struct AdshKey {
	uint32_t size = 0;
	uint64_t w0 = 0;
	uint64_t w1 = 0;
	uint64_t w2 = 0;
	uint64_t w3 = 0;

	static AdshKey From(const duckdb::string_t &adsh) {
		AdshKey key;
		key.size = static_cast<uint32_t>(adsh.GetSize());
		const auto *data = adsh.GetData();
		if (data == nullptr || key.size == 0) {
			return key;
		}
		const auto copy_word = [&](duckdb::idx_t offset, uint64_t &word) {
			if (key.size <= offset) {
				return;
			}
			std::memset(&word, 0, sizeof(word));
			std::memcpy(&word, data + offset, std::min<duckdb::idx_t>(key.size - offset, sizeof(word)));
		};
		copy_word(0, key.w0);
		copy_word(8, key.w1);
		copy_word(16, key.w2);
		copy_word(24, key.w3);
		return key;
	}

	bool operator==(const AdshKey &other) const {
		return size == other.size && w0 == other.w0 && w1 == other.w1 && w2 == other.w2 && w3 == other.w3;
	}
};

struct AdshKeyHash {
	size_t operator()(const AdshKey &key) const noexcept {
		return key.w0 ^ (key.w1 << 1) ^ (key.w2 << 2) ^ (key.w3 << 3) ^ key.size;
	}
};

class AdshInternTable {
public:
	explicit AdshInternTable(size_t reserve_hint) {
		lookup.reserve(reserve_hint);
	}

	uint32_t Intern(const duckdb::string_t &adsh) {
		const AdshKey key = AdshKey::From(adsh);
		const auto it = lookup.find(key);
		if (it != lookup.end()) {
			return it->second;
		}
		const uint32_t index = next_index++;
		lookup.emplace(key, index);
		return index;
	}

private:
	std::unordered_map<AdshKey, uint32_t, AdshKeyHash> lookup;
	uint32_t next_index = 0;
};

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

NativeScanSpec CreateAuxPreScanSpec(SecEdgarQ1PartitionKey partition_eq = std::numeric_limits<SecEdgarQ1PartitionKey>::max()) {
	NativeScanSpec scan_spec = CreateSecEdgarQ1ScanSpec();
	if (partition_eq != std::numeric_limits<SecEdgarQ1PartitionKey>::max()) {
		(void)partition_eq;
	}
	return scan_spec;
}

void ConsumePreChunk(duckdb::DataChunk &chunk, std::unordered_map<SecEdgarQ1PartitionKey, SecEdgarQ1GroupPartition> &groups,
                     AdshInternTable &adsh_intern) {
	duckdb::UnifiedVectorFormat stmt_fmt;
	duckdb::UnifiedVectorFormat rfile_fmt;
	duckdb::UnifiedVectorFormat adsh_fmt;
	duckdb::UnifiedVectorFormat line_fmt;
	chunk.data[SecEdgarQ1PreChunkLayout::STMT_INDEX].ToUnifiedFormat(chunk.size(), stmt_fmt);
	chunk.data[SecEdgarQ1PreChunkLayout::RFILE_INDEX].ToUnifiedFormat(chunk.size(), rfile_fmt);
	chunk.data[SecEdgarQ1PreChunkLayout::ADSH_INDEX].ToUnifiedFormat(chunk.size(), adsh_fmt);
	chunk.data[SecEdgarQ1PreChunkLayout::LINE_INDEX].ToUnifiedFormat(chunk.size(), line_fmt);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto stmt_idx = stmt_fmt.sel->get_index(row);
		const auto rfile_idx = rfile_fmt.sel->get_index(row);
		const auto adsh_idx = adsh_fmt.sel->get_index(row);
		const auto line_idx = line_fmt.sel->get_index(row);
		if (!stmt_fmt.validity.RowIsValid(stmt_idx)) {
			continue;
		}
		const auto &stmt = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(stmt_fmt)[stmt_idx];
		const auto &rfile = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(rfile_fmt)[rfile_idx];
		const bool rfile_valid = rfile_fmt.validity.RowIsValid(rfile_idx);
		const auto partition = PartitionKeyFromStmtRfile(stmt, rfile, !rfile_valid);
		if (partition == std::numeric_limits<SecEdgarQ1PartitionKey>::max()) {
			continue;
		}
		auto &group = groups[partition];
		if (group.stmt_label.empty()) {
			group.stmt_label = ToStoredString(stmt);
			group.rfile_label = ToStoredString(rfile);
			group.rfile_is_null = !rfile_valid;
		}
		int32_t line = 0;
		if (line_fmt.validity.RowIsValid(line_idx)) {
			line = duckdb::UnifiedVectorFormat::GetData<int32_t>(line_fmt)[line_idx];
		}
		uint32_t adsh_intern_id = std::numeric_limits<uint32_t>::max();
		if (adsh_fmt.validity.RowIsValid(adsh_idx)) {
			const auto &adsh = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(adsh_fmt)[adsh_idx];
			adsh_intern_id = adsh_intern.Intern(adsh);
		}
		ApplySecEdgarQ1RowContribution(group, line, adsh_intern_id, 1);
	}
}

void ScanPre(duckdb::ClientContext &context, std::unordered_map<SecEdgarQ1PartitionKey, SecEdgarQ1GroupPartition> &groups) {
	AdshInternTable adsh_intern(2048);
	auto bind_data = BindNativeScan(context, CreateAuxPreScanSpec());
	auto source = CreateNativeChunkSource(context, *bind_data);
	while (source->Next()) {
		ConsumePreChunk(source->Chunk(), groups, adsh_intern);
	}
}

aux_state::PartitionedStateStore &GetSecEdgarQ1Store(duckdb::ClientContext &context) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	return registry->GetStore(kSecEdgarQ1AuxAcceleratorId);
}

void PublishGroups(duckdb::ClientContext &context,
                   const std::unordered_map<SecEdgarQ1PartitionKey, SecEdgarQ1GroupPartition> &groups) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	auto &store = registry->GetStore(kSecEdgarQ1AuxAcceleratorId);
	const auto publish_epoch = registry->GetMaintenanceState(kSecEdgarQ1AuxAcceleratorId).BumpCommittedMutationEpoch();
	for (const auto &entry : groups) {
		store.PublishPartition(entry.first, std::make_shared<SecEdgarQ1GroupPartition>(entry.second), publish_epoch);
	}
}

bool PartitionMatchesRow(SecEdgarQ1PartitionKey partition, const duckdb::string_t &stmt, const duckdb::string_t &rfile,
                         bool rfile_is_null) {
	return PartitionKeyFromStmtRfile(stmt, rfile, rfile_is_null) == partition;
}

void ScanPreForPartition(duckdb::ClientContext &context, SecEdgarQ1PartitionKey partition,
                         SecEdgarQ1GroupPartition &group) {
	AdshInternTable adsh_intern(2048);
	auto bind_data = BindNativeScan(context, CreateAuxPreScanSpec());
	auto source = CreateNativeChunkSource(context, *bind_data);
	while (source->Next()) {
		auto &chunk = source->Chunk();
		duckdb::UnifiedVectorFormat stmt_fmt;
		duckdb::UnifiedVectorFormat rfile_fmt;
		duckdb::UnifiedVectorFormat adsh_fmt;
		duckdb::UnifiedVectorFormat line_fmt;
		chunk.data[SecEdgarQ1PreChunkLayout::STMT_INDEX].ToUnifiedFormat(chunk.size(), stmt_fmt);
		chunk.data[SecEdgarQ1PreChunkLayout::RFILE_INDEX].ToUnifiedFormat(chunk.size(), rfile_fmt);
		chunk.data[SecEdgarQ1PreChunkLayout::ADSH_INDEX].ToUnifiedFormat(chunk.size(), adsh_fmt);
		chunk.data[SecEdgarQ1PreChunkLayout::LINE_INDEX].ToUnifiedFormat(chunk.size(), line_fmt);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto stmt_idx = stmt_fmt.sel->get_index(row);
			const auto rfile_idx = rfile_fmt.sel->get_index(row);
			const auto adsh_idx = adsh_fmt.sel->get_index(row);
			const auto line_idx = line_fmt.sel->get_index(row);
			if (!stmt_fmt.validity.RowIsValid(stmt_idx)) {
				continue;
			}
			const auto &stmt = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(stmt_fmt)[stmt_idx];
			const auto &rfile = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(rfile_fmt)[rfile_idx];
			const bool rfile_valid = rfile_fmt.validity.RowIsValid(rfile_idx);
			if (!PartitionMatchesRow(partition, stmt, rfile, !rfile_valid)) {
				continue;
			}
			if (group.stmt_label.empty()) {
				group.stmt_label = ToStoredString(stmt);
				group.rfile_label = ToStoredString(rfile);
				group.rfile_is_null = !rfile_valid;
			}
			int32_t line = 0;
			if (line_fmt.validity.RowIsValid(line_idx)) {
				line = duckdb::UnifiedVectorFormat::GetData<int32_t>(line_fmt)[line_idx];
			}
			uint32_t adsh_intern_id = std::numeric_limits<uint32_t>::max();
			if (adsh_fmt.validity.RowIsValid(adsh_idx)) {
				const auto &adsh = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(adsh_fmt)[adsh_idx];
				adsh_intern_id = adsh_intern.Intern(adsh);
			}
			ApplySecEdgarQ1RowContribution(group, line, adsh_intern_id, 1);
		}
	}
}

} // namespace

namespace {
SecEdgarQ1AuxDiagnostics g_sec_edgar_q1_aux_diagnostics_mut;
} // namespace

void ResetSecEdgarQ1AuxDiagnostics() {
	g_sec_edgar_q1_aux_diagnostics_mut = SecEdgarQ1AuxDiagnostics {};
}

void UpdateSecEdgarQ1AuxDiagnostics(const SecEdgarQ1AuxDiagnostics &diagnostics) {
	g_sec_edgar_q1_aux_diagnostics_mut = diagnostics;
}

const SecEdgarQ1AuxDiagnostics &GetSecEdgarQ1AuxDiagnostics() {
	return g_sec_edgar_q1_aux_diagnostics_mut;
}

aux_state::ExecutionGateResult
CheckSecEdgarQ1GroupExecutionGate(const aux_state::PartitionedStateStore &store,
                                  const aux_state::TxnInvalidationBuffer &txn_buffer,
                                  uint64_t reader_visible_mutation_epoch) {
	aux_state::ExecutionGateResult result;
	if (txn_buffer.IsAllBypassed()) {
		result.bypassed_by_txn = true;
		result.committed_invalid = true;
		return result;
	}
	bool saw_valid = false;
	store.ForEachSlot([&](SecEdgarQ1PartitionKey key, aux_state::PartitionStatus status,
	                      const std::shared_ptr<const void> &payload, uint64_t valid_through_mutation_epoch) {
		if (status != aux_state::PartitionStatus::Valid || !payload) {
			if (status == aux_state::PartitionStatus::Invalid) {
				result.committed_invalid = true;
			}
			return;
		}
		const auto &partition = *std::static_pointer_cast<const SecEdgarQ1GroupPartition>(payload);
		if (partition.cnt <= 0) {
			return;
		}
		saw_valid = true;
		if (txn_buffer.IsPartitionBypassed(key)) {
			result.bypassed_by_txn = true;
		}
		if (valid_through_mutation_epoch > reader_visible_mutation_epoch) {
			result.committed_invalid = true;
		}
	});
	if (!saw_valid) {
		result.committed_invalid = true;
	}
	if (result.bypassed_by_txn || result.committed_invalid) {
		return result;
	}
	result.use_aux = true;
	return result;
}

void BuildSecEdgarQ1AuxState(duckdb::ClientContext &context) {
	const auto start = std::chrono::steady_clock::now();
	std::unordered_map<SecEdgarQ1PartitionKey, SecEdgarQ1GroupPartition> groups;
	ScanPre(context, groups);
	PublishGroups(context, groups);
	RecordSecEdgarQ1Rebuild();
	g_sec_edgar_q1_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void RebuildSecEdgarQ1AuxPartition(duckdb::ClientContext &context, SecEdgarQ1PartitionKey partition) {
	const auto start = std::chrono::steady_clock::now();
	SecEdgarQ1GroupPartition group;
	ScanPreForPartition(context, partition, group);
	auto &store = GetSecEdgarQ1Store(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	const auto publish_epoch = registry->GetMaintenanceState(kSecEdgarQ1AuxAcceleratorId).BumpCommittedMutationEpoch();
	store.PublishPartition(partition, std::make_shared<SecEdgarQ1GroupPartition>(group), publish_epoch);
	RecordSecEdgarQ1Rebuild();
	g_sec_edgar_q1_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void InvalidateSecEdgarQ1AuxStateForTests(duckdb::ClientContext &context) {
	GetSecEdgarQ1Store(context).InvalidateAll();
}

std::shared_ptr<const SecEdgarQ1GroupPartition> TryGetReadableSecEdgarQ1GroupPartition(duckdb::ClientContext &context,
                                                                                       SecEdgarQ1PartitionKey partition) {
	EnsurePendingSecEdgarQ1RebuildApplied(context);
	auto &store = GetSecEdgarQ1Store(context);
	auto payload = store.GetReadablePayload(partition);
	if (!payload) {
		return nullptr;
	}
	return std::static_pointer_cast<const SecEdgarQ1GroupPartition>(payload);
}

std::vector<SecEdgarQ1OutputRow> CollectSecEdgarQ1AuxResult(duckdb::ClientContext &context) {
	EnsurePendingSecEdgarQ1RebuildApplied(context);
	auto &store = GetSecEdgarQ1Store(context);
	std::vector<SecEdgarQ1OutputRow> rows;
	int64_t partitions_read = 0;
	store.ForEachSlot([&](SecEdgarQ1PartitionKey key, aux_state::PartitionStatus status,
	                      const std::shared_ptr<const void> &payload, uint64_t) {
		(void)key;
		if (status != aux_state::PartitionStatus::Valid || !payload) {
			return;
		}
		const auto &partition = *std::static_pointer_cast<const SecEdgarQ1GroupPartition>(payload);
		if (partition.cnt <= 0) {
			return;
		}
		partitions_read++;
		rows.push_back(FinalizeSecEdgarQ1GroupPartition(partition));
	});
	g_sec_edgar_q1_aux_diagnostics_mut.partitions_read = partitions_read;
	return FinalizeAndSortSecEdgarQ1Rows(std::move(rows));
}

} // namespace accelerator_codegen
