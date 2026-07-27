#include "accelerator_codegen/tpch_q1_aux.hpp"
#include "accelerator_codegen/tpch_q1_rebuild_policy.hpp"
#include "accelerator_codegen/tpch_q1_write_policy.hpp"

#include <chrono>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/chunk_source.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/tpch_q1.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

NativeScanSpec CreateAuxLineitemScanSpec(const duckdb::date_t *shipdate_eq = nullptr) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	    {"l_quantity", "quantity", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_discount", "discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_tax", "tax", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_returnflag", "returnflag", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"l_linestatus", "linestatus", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	if (shipdate_eq) {
		scan_spec.filters = {
		    {"l_shipdate", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value::DATE(*shipdate_eq)},
		};
	}
	return scan_spec;
}

void ConsumeLineitemChunk(duckdb::DataChunk &chunk, std::unordered_map<Q1PartitionKey, Q1DayPartition> &days) {
	duckdb::UnifiedVectorFormat shipdates;
	duckdb::UnifiedVectorFormat quantities;
	duckdb::UnifiedVectorFormat prices;
	duckdb::UnifiedVectorFormat discounts;
	duckdb::UnifiedVectorFormat taxes;
	duckdb::UnifiedVectorFormat returnflags;
	duckdb::UnifiedVectorFormat linestatuses;
	chunk.data[0].ToUnifiedFormat(chunk.size(), shipdates);
	chunk.data[1].ToUnifiedFormat(chunk.size(), quantities);
	chunk.data[2].ToUnifiedFormat(chunk.size(), prices);
	chunk.data[3].ToUnifiedFormat(chunk.size(), discounts);
	chunk.data[4].ToUnifiedFormat(chunk.size(), taxes);
	chunk.data[5].ToUnifiedFormat(chunk.size(), returnflags);
	chunk.data[6].ToUnifiedFormat(chunk.size(), linestatuses);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto shipdate_idx = shipdates.sel->get_index(row);
		const auto qty_idx = quantities.sel->get_index(row);
		const auto price_idx = prices.sel->get_index(row);
		const auto discount_idx = discounts.sel->get_index(row);
		const auto tax_idx = taxes.sel->get_index(row);
		const auto rf_idx = returnflags.sel->get_index(row);
		const auto ls_idx = linestatuses.sel->get_index(row);
		if (!shipdates.validity.RowIsValid(shipdate_idx) || !quantities.validity.RowIsValid(qty_idx) ||
		    !prices.validity.RowIsValid(price_idx) || !discounts.validity.RowIsValid(discount_idx) ||
		    !taxes.validity.RowIsValid(tax_idx) || !returnflags.validity.RowIsValid(rf_idx) ||
		    !linestatuses.validity.RowIsValid(ls_idx)) {
			continue;
		}
		const auto shipdate = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdates)[shipdate_idx];
		const auto qty = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantities)[qty_idx];
		const auto price = duckdb::UnifiedVectorFormat::GetData<int64_t>(prices)[price_idx];
		const auto discount = duckdb::UnifiedVectorFormat::GetData<int64_t>(discounts)[discount_idx];
		const auto tax = duckdb::UnifiedVectorFormat::GetData<int64_t>(taxes)[tax_idx];
		const auto &returnflag = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(returnflags)[rf_idx];
		const auto &linestatus = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(linestatuses)[ls_idx];
		if (returnflag.GetSize() != 1 || linestatus.GetSize() != 1) {
			continue;
		}
		const auto partition = PartitionKeyFromShipdate(shipdate);
		const auto group_key = PackQ1GroupKey(returnflag.GetDataUnsafe()[0], linestatus.GetDataUnsafe()[0]);
		auto &group = days[partition].groups[group_key];
		ApplyQ1RowContribution(group, qty, price, discount, tax, 1);
	}
}

void ScanLineitem(duckdb::ClientContext &context, std::unordered_map<Q1PartitionKey, Q1DayPartition> &days,
                  const duckdb::date_t *shipdate_eq) {
	auto bind_data = BindNativeScan(context, shipdate_eq ? CreateAuxLineitemScanSpec(shipdate_eq) : CreateAuxLineitemScanSpec());
	auto source = CreateNativeChunkSource(context, *bind_data);
	while (source->Next()) {
		ConsumeLineitemChunk(source->Chunk(), days);
	}
}

aux_state::PartitionedStateStore &GetQ1Store(duckdb::ClientContext &context) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	return registry->GetStore(kQ1AuxAcceleratorId);
}

void PublishDays(duckdb::ClientContext &context, const std::unordered_map<Q1PartitionKey, Q1DayPartition> &days) {
	auto &store = GetQ1Store(context);
	for (const auto &entry : days) {
		store.PublishPartition(entry.first, std::make_shared<Q1DayPartition>(entry.second));
	}
}

duckdb::date_t PartitionKeyToShipdate(Q1PartitionKey key) {
	return duckdb::date_t {key};
}

} // namespace

namespace {
Q1AuxDiagnostics g_q1_aux_diagnostics_mut;
} // namespace

void ResetQ1AuxDiagnostics() {
	g_q1_aux_diagnostics_mut = Q1AuxDiagnostics {};
}

void UpdateQ1AuxDiagnostics(const Q1AuxDiagnostics &diagnostics) {
	g_q1_aux_diagnostics_mut = diagnostics;
}

const Q1AuxDiagnostics &GetQ1AuxDiagnostics() {
	return g_q1_aux_diagnostics_mut;
}

aux_state::ExecutionGateResult CheckQ1PrefixExecutionGate(const aux_state::PartitionedStateStore &store,
                                                            const aux_state::TxnInvalidationBuffer &txn_buffer,
                                                            Q1PartitionKey cutoff) {
	aux_state::ExecutionGateResult result;
	if (txn_buffer.IsAllBypassed()) {
		result.bypassed_by_txn = true;
		result.committed_invalid = true;
		return result;
	}
	store.ForEachSlot([&](Q1PartitionKey key, aux_state::PartitionStatus status, const std::shared_ptr<const void> &) {
		if (key > cutoff) {
			return;
		}
		if (txn_buffer.IsPartitionBypassed(key)) {
			result.bypassed_by_txn = true;
		}
		if (status == aux_state::PartitionStatus::Invalid) {
			result.committed_invalid = true;
		}
	});
	if (result.bypassed_by_txn || result.committed_invalid) {
		return result;
	}
	result.use_aux = true;
	return result;
}

void BuildQ1AuxState(duckdb::ClientContext &context) {
	const auto start = std::chrono::steady_clock::now();
	std::unordered_map<Q1PartitionKey, Q1DayPartition> days;
	ScanLineitem(context, days, nullptr);
	PublishDays(context, days);
	RecordQ1Rebuild();
	g_q1_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void RebuildQ1AuxPartition(duckdb::ClientContext &context, Q1PartitionKey partition) {
	const auto start = std::chrono::steady_clock::now();
	std::unordered_map<Q1PartitionKey, Q1DayPartition> days;
	const auto shipdate = PartitionKeyToShipdate(partition);
	ScanLineitem(context, days, &shipdate);
	auto &store = GetQ1Store(context);
	auto it = days.find(partition);
	if (it == days.end()) {
		store.PublishPartition(partition, std::make_shared<Q1DayPartition>());
	} else {
		store.PublishPartition(partition, std::make_shared<Q1DayPartition>(it->second));
	}
	RecordQ1Rebuild();
	g_q1_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void InvalidateQ1AuxStateForTests(duckdb::ClientContext &context) {
	GetQ1Store(context).InvalidateAll();
}

std::shared_ptr<const Q1DayPartition> TryGetReadableDayPartition(duckdb::ClientContext &context,
                                                                   Q1PartitionKey partition) {
	EnsurePendingQ1RebuildApplied(context);
	auto &store = GetQ1Store(context);
	auto payload = store.GetReadablePayload(partition);
	if (!payload) {
		return nullptr;
	}
	return std::static_pointer_cast<const Q1DayPartition>(payload);
}

std::vector<Q1OutputRow> MergeQ1PartitionsUpTo(duckdb::ClientContext &context, Q1PartitionKey cutoff) {
	EnsurePendingQ1RebuildApplied(context);
	auto &store = GetQ1Store(context);
	std::unordered_map<Q1GroupKey, Q1MergedGroupStats> merged;
	int64_t partitions_merged = 0;
	store.ForEachSlot([&](Q1PartitionKey key, aux_state::PartitionStatus status, const std::shared_ptr<const void> &payload) {
		if (key > cutoff || status != aux_state::PartitionStatus::Valid || !payload) {
			return;
		}
		const auto &day = *std::static_pointer_cast<const Q1DayPartition>(payload);
		partitions_merged++;
		for (const auto &group_entry : day.groups) {
			MergeQ1DayGroupStats(merged[group_entry.first], group_entry.second);
		}
	});
	g_q1_aux_diagnostics_mut.partitions_merged = partitions_merged;
	return FinalizeQ1MergedGroups(merged);
}

} // namespace accelerator_codegen
