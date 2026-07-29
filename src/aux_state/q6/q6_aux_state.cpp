#include "accelerator_codegen/tpch_q6_aux.hpp"
#include "accelerator_codegen/tpch_q6_rebuild_policy.hpp"
#include "accelerator_codegen/tpch_q6_write_policy.hpp"

#include <chrono>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/chunk_source.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

constexpr const char *kQ6AuxId = "tpch_q6_aux";

using DayBuildMap = std::unordered_map<Q6PartitionKey, Q6DayPartition>;

NativeScanSpec CreateAuxLineitemScanSpec(const duckdb::date_t *shipdate_eq = nullptr) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	    {"l_discount", "discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_quantity", "quantity", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	};
	if (shipdate_eq) {
		scan_spec.filters = {
		    {"l_shipdate", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value::DATE(*shipdate_eq)},
		};
	}
	return scan_spec;
}

void ConsumeLineitemChunk(duckdb::DataChunk &chunk, DayBuildMap &days) {
	duckdb::UnifiedVectorFormat shipdates;
	duckdb::UnifiedVectorFormat discounts;
	duckdb::UnifiedVectorFormat quantities;
	duckdb::UnifiedVectorFormat extendedprices;
	chunk.data[0].ToUnifiedFormat(chunk.size(), shipdates);
	chunk.data[1].ToUnifiedFormat(chunk.size(), discounts);
	chunk.data[2].ToUnifiedFormat(chunk.size(), quantities);
	chunk.data[3].ToUnifiedFormat(chunk.size(), extendedprices);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto sd_idx = shipdates.sel->get_index(row);
		const auto ds_idx = discounts.sel->get_index(row);
		const auto qt_idx = quantities.sel->get_index(row);
		const auto ep_idx = extendedprices.sel->get_index(row);
		if (!shipdates.validity.RowIsValid(sd_idx) || !discounts.validity.RowIsValid(ds_idx) ||
		    !quantities.validity.RowIsValid(qt_idx) || !extendedprices.validity.RowIsValid(ep_idx)) {
			continue;
		}
		const auto shipdate = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdates)[sd_idx];
		const auto discount_raw = duckdb::UnifiedVectorFormat::GetData<int64_t>(discounts)[ds_idx];
		const auto quantity_raw = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantities)[qt_idx];
		const auto extendedprice_raw = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprices)[ep_idx];
		const auto partition = Q6PartitionKeyFromShipdate(shipdate);
		auto &day = days[partition];
		ApplyQ6RowContribution(day, static_cast<int8_t>(discount_raw), quantity_raw,
		                       Q6RowContributionRaw(extendedprice_raw, discount_raw), 1);
	}
}

void ScanLineitem(duckdb::ClientContext &context, DayBuildMap &days, const duckdb::date_t *shipdate_eq) {
	auto bind_data = BindNativeScan(context, shipdate_eq ? CreateAuxLineitemScanSpec(shipdate_eq) : CreateAuxLineitemScanSpec());
	auto source = CreateNativeChunkSource(context, *bind_data);
	while (source->Next()) {
		ConsumeLineitemChunk(source->Chunk(), days);
	}
}

aux_state::PartitionedStateStore &GetQ6Store(duckdb::ClientContext &context) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	return registry->GetStore(kQ6AuxId);
}

void PublishDays(duckdb::ClientContext &context, const DayBuildMap &days) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	auto &store = GetQ6Store(context);
	const auto publish_epoch = registry->GetMaintenanceState(kQ6AuxId).BumpCommittedMutationEpoch();
	for (const auto &entry : days) {
		store.PublishPartition(entry.first, std::make_shared<Q6DayPartition>(entry.second), publish_epoch);
	}
}

DayBuildMap BuildAllDays(duckdb::ClientContext &context) {
	DayBuildMap days;
	ScanLineitem(context, days, nullptr);
	return days;
}

DayBuildMap BuildOneDay(duckdb::ClientContext &context, Q6PartitionKey partition) {
	DayBuildMap days;
	const auto shipdate = Q6ShipdateFromPartitionKey(partition);
	ScanLineitem(context, days, &shipdate);
	return days;
}

} // namespace

namespace {
Q6AuxDiagnostics g_q6_aux_diagnostics_mut;
} // namespace

void ResetQ6AuxDiagnostics() {
	g_q6_aux_diagnostics_mut = Q6AuxDiagnostics {};
}

void UpdateQ6AuxDiagnostics(const Q6AuxDiagnostics &diagnostics) {
	g_q6_aux_diagnostics_mut = diagnostics;
}

const Q6AuxDiagnostics &GetQ6AuxDiagnostics() {
	return g_q6_aux_diagnostics_mut;
}

aux_state::ExecutionGateResult CheckQ6WindowExecutionGate(const aux_state::PartitionedStateStore &store,
                                                           const aux_state::TxnInvalidationBuffer &txn_buffer,
                                                           Q6PartitionKey start_partition,
                                                           Q6PartitionKey end_partition,
                                                           uint64_t reader_visible_mutation_epoch) {
	aux_state::ExecutionGateResult result;
	if (txn_buffer.IsAllBypassed()) {
		result.bypassed_by_txn = true;
		result.committed_invalid = true;
		return result;
	}
	if (store.TotalPartitionSlots() == 0) {
		result.committed_invalid = true;
		return result;
	}
	for (Q6PartitionKey key = start_partition; key < end_partition; ++key) {
		if (txn_buffer.IsPartitionBypassed(key)) {
			result.bypassed_by_txn = true;
		}
		const auto status = store.GetStatus(key);
		if (status == aux_state::PartitionStatus::Absent) {
			continue;
		}
		if (status == aux_state::PartitionStatus::Invalid) {
			result.committed_invalid = true;
		}
		(void)reader_visible_mutation_epoch;
	}
	if (result.bypassed_by_txn || result.committed_invalid) {
		return result;
	}
	result.use_aux = true;
	return result;
}

void BuildQ6AuxState(duckdb::ClientContext &context) {
	const auto start = std::chrono::steady_clock::now();
	auto days = BuildAllDays(context);
	PublishDays(context, days);
	RecordQ6Rebuild();
	g_q6_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void RebuildQ6AuxPartition(duckdb::ClientContext &context, Q6PartitionKey partition) {
	const auto start = std::chrono::steady_clock::now();
	auto days = BuildOneDay(context, partition);
	auto &store = GetQ6Store(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	const auto publish_epoch = registry->GetMaintenanceState(kQ6AuxId).BumpCommittedMutationEpoch();
	auto it = days.find(partition);
	if (it == days.end()) {
		store.PublishPartition(partition, std::make_shared<Q6DayPartition>(), publish_epoch);
	} else {
		store.PublishPartition(partition, std::make_shared<Q6DayPartition>(it->second), publish_epoch);
	}
	RecordQ6Rebuild();
	g_q6_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void InvalidateQ6AuxStateForTests(duckdb::ClientContext &context) {
	GetQ6Store(context).InvalidateAll();
}

std::shared_ptr<const Q6DayPartition> TryGetReadableQ6DayPartition(duckdb::ClientContext &context,
                                                                    Q6PartitionKey partition) {
	EnsurePendingQ6RebuildApplied(context);
	auto &store = GetQ6Store(context);
	auto payload = store.GetReadablePayload(partition);
	if (!payload) {
		return nullptr;
	}
	return std::static_pointer_cast<const Q6DayPartition>(payload);
}

Q6AccT ComposeQ6AuxRevenue(duckdb::ClientContext &context, const Q6Parameters &parameters) {
	const auto start_partition = Q6PartitionKeyFromShipdate(parameters.start_date);
	const auto end_partition = Q6PartitionKeyFromShipdate(parameters.end_date);
	auto &store = GetQ6Store(context);
	Q6AccT revenue = static_cast<Q6AccT>(0);
	int64_t partitions_merged = 0;
	store.VisitReadablePartitionsInRange(start_partition, end_partition,
	                                     [&](Q6PartitionKey key, const std::shared_ptr<const void> &payload) {
		                                     (void)key;
		                                     const auto &day = *std::static_pointer_cast<const Q6DayPartition>(payload);
		                                     partitions_merged++;
		                                     MergeQ6DayRevenue(revenue, day, parameters.discount_lower_raw,
		                                                       parameters.discount_upper_raw, parameters.quantity_upper_raw);
	                                     });
	g_q6_aux_diagnostics_mut.partitions_merged = partitions_merged;
	g_q6_aux_diagnostics_mut.start_partition = start_partition;
	g_q6_aux_diagnostics_mut.end_partition = end_partition;
	return revenue;
}

} // namespace accelerator_codegen
