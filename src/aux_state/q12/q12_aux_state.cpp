#include "accelerator_codegen/tpch_q12_aux.hpp"
#include "accelerator_codegen/tpch_q12_rebuild_policy.hpp"
#include "accelerator_codegen/tpch_q12_write_policy.hpp"

#include <chrono>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/execution_gate.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/chunk_source.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/tpch_q12.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

constexpr const char *kQ12AuxId = "tpch_q12_aux";
constexpr size_t kDefaultOrderCapacity = 6000001;

class OrderPriorityStore {
public:
	Q12PriorityClass Probe(int64_t orderkey) const {
		if (orderkey < 0) {
			const auto it = overflow.find(orderkey);
			return it == overflow.end() ? Q12PriorityClass::Absent : it->second;
		}
		const auto key = static_cast<size_t>(orderkey);
		if (key < classes.size()) {
			return static_cast<Q12PriorityClass>(classes[key]);
		}
		const auto it = overflow.find(orderkey);
		return it == overflow.end() ? Q12PriorityClass::Absent : it->second;
	}

	void Store(int64_t orderkey, Q12PriorityClass priority) {
		if (orderkey < 0 || static_cast<size_t>(orderkey) >= classes.size()) {
			overflow[orderkey] = priority;
			return;
		}
		classes[static_cast<size_t>(orderkey)] = static_cast<uint8_t>(priority);
	}

private:
	std::vector<uint8_t> classes =
	    std::vector<uint8_t>(kDefaultOrderCapacity, static_cast<uint8_t>(Q12PriorityClass::Absent));
	std::unordered_map<int64_t, Q12PriorityClass> overflow;
};

using DayBuildMap = std::unordered_map<Q12PartitionKey, Q12DayPartition>;

NativeScanSpec CreateAuxOrdersScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "orders";
	scan_spec.projected_columns = {
	    {"o_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_orderpriority", "orderpriority", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

NativeScanSpec CreateAuxLineitemScanSpec(const duckdb::date_t *receiptdate_eq = nullptr) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_shipmode", "shipmode", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"l_commitdate", "commitdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	    {"l_receiptdate", "receiptdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	    {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	};
	if (receiptdate_eq) {
		scan_spec.filters = {
		    {"l_receiptdate", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value::DATE(*receiptdate_eq)},
		};
	}
	return scan_spec;
}

void ConsumeOrdersChunk(duckdb::DataChunk &chunk, OrderPriorityStore &store) {
	duckdb::UnifiedVectorFormat orderkeys;
	duckdb::UnifiedVectorFormat priorities;
	chunk.data[0].ToUnifiedFormat(chunk.size(), orderkeys);
	chunk.data[1].ToUnifiedFormat(chunk.size(), priorities);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto ok_idx = orderkeys.sel->get_index(row);
		const auto pr_idx = priorities.sel->get_index(row);
		if (!orderkeys.validity.RowIsValid(ok_idx)) {
			continue;
		}
		const auto orderkey = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkeys)[ok_idx];
		Q12PriorityClass priority = Q12PriorityClass::Neither;
		if (priorities.validity.RowIsValid(pr_idx)) {
			const auto &value = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(priorities)[pr_idx];
			priority = ClassifyQ12Priority(value, false);
		}
		store.Store(orderkey, priority);
	}
}

void ConsumeLineitemChunk(duckdb::DataChunk &chunk, const OrderPriorityStore &store, DayBuildMap &days) {
	duckdb::UnifiedVectorFormat orderkeys;
	duckdb::UnifiedVectorFormat shipmodes;
	duckdb::UnifiedVectorFormat commitdates;
	duckdb::UnifiedVectorFormat receiptdates;
	duckdb::UnifiedVectorFormat shipdates;
	chunk.data[0].ToUnifiedFormat(chunk.size(), orderkeys);
	chunk.data[1].ToUnifiedFormat(chunk.size(), shipmodes);
	chunk.data[2].ToUnifiedFormat(chunk.size(), commitdates);
	chunk.data[3].ToUnifiedFormat(chunk.size(), receiptdates);
	chunk.data[4].ToUnifiedFormat(chunk.size(), shipdates);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto ok_idx = orderkeys.sel->get_index(row);
		const auto sm_idx = shipmodes.sel->get_index(row);
		const auto cd_idx = commitdates.sel->get_index(row);
		const auto rd_idx = receiptdates.sel->get_index(row);
		const auto sd_idx = shipdates.sel->get_index(row);
		if (!orderkeys.validity.RowIsValid(ok_idx) || !shipmodes.validity.RowIsValid(sm_idx) ||
		    !commitdates.validity.RowIsValid(cd_idx) || !receiptdates.validity.RowIsValid(rd_idx) ||
		    !shipdates.validity.RowIsValid(sd_idx)) {
			continue;
		}
		const auto orderkey = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkeys)[ok_idx];
		const auto priority = store.Probe(orderkey);
		if (priority == Q12PriorityClass::Absent) {
			continue;
		}
		const auto commitdate = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(commitdates)[cd_idx];
		const auto receiptdate = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(receiptdates)[rd_idx];
		const auto shipdate = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdates)[sd_idx];
		if (!Q12LineitemQualifies(commitdate, receiptdate, shipdate)) {
			continue;
		}
		const auto &shipmode = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(shipmodes)[sm_idx];
		const auto partition = PartitionKeyFromReceiptdate(receiptdate);
		auto &stats = days[partition].shipmodes[shipmode.GetString()];
		ApplyQ12LineitemContribution(stats, priority, 1);
	}
}

void ScanOrders(duckdb::ClientContext &context, OrderPriorityStore &store) {
	auto bind_data = BindNativeScan(context, CreateAuxOrdersScanSpec());
	auto source = CreateNativeChunkSource(context, *bind_data);
	while (source->Next()) {
		ConsumeOrdersChunk(source->Chunk(), store);
	}
}

void ScanLineitem(duckdb::ClientContext &context, const OrderPriorityStore &store, DayBuildMap &days,
                  const duckdb::date_t *receiptdate_eq) {
	auto bind_data = BindNativeScan(context,
	                                receiptdate_eq ? CreateAuxLineitemScanSpec(receiptdate_eq) : CreateAuxLineitemScanSpec());
	auto source = CreateNativeChunkSource(context, *bind_data);
	while (source->Next()) {
		ConsumeLineitemChunk(source->Chunk(), store, days);
	}
}

aux_state::PartitionedStateStore &GetQ12Store(duckdb::ClientContext &context) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	return registry->GetStore(kQ12AuxId);
}

void PublishDays(duckdb::ClientContext &context, const DayBuildMap &days) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	auto &store = GetQ12Store(context);
	const auto publish_epoch = registry->GetMaintenanceState(kQ12AuxId).BumpCommittedMutationEpoch();
	for (const auto &entry : days) {
		store.PublishPartition(entry.first, std::make_shared<Q12DayPartition>(entry.second), publish_epoch);
	}
}

DayBuildMap BuildAllDays(duckdb::ClientContext &context) {
	OrderPriorityStore store;
	DayBuildMap days;
	ScanOrders(context, store);
	ScanLineitem(context, store, days, nullptr);
	return days;
}

DayBuildMap BuildOneDay(duckdb::ClientContext &context, Q12PartitionKey partition) {
	OrderPriorityStore store;
	DayBuildMap days;
	ScanOrders(context, store);
	const auto receiptdate = ReceiptdateFromPartitionKey(partition);
	ScanLineitem(context, store, days, &receiptdate);
	return days;
}

} // namespace

namespace {
Q12AuxDiagnostics g_q12_aux_diagnostics_mut;
} // namespace

void ResetQ12AuxDiagnostics() {
	g_q12_aux_diagnostics_mut = Q12AuxDiagnostics {};
}

void UpdateQ12AuxDiagnostics(const Q12AuxDiagnostics &diagnostics) {
	g_q12_aux_diagnostics_mut = diagnostics;
}

const Q12AuxDiagnostics &GetQ12AuxDiagnostics() {
	return g_q12_aux_diagnostics_mut;
}

Q12PartitionKey PartitionKeyFromReceiptdate(duckdb::date_t receiptdate) {
	return static_cast<Q12PartitionKey>(receiptdate.days);
}

duckdb::date_t ReceiptdateFromPartitionKey(Q12PartitionKey key) {
	return duckdb::date_t {static_cast<int32_t>(key)};
}

aux_state::ExecutionGateResult CheckQ12WindowExecutionGate(const aux_state::PartitionedStateStore &store,
                                                           const aux_state::TxnInvalidationBuffer &txn_buffer,
                                                           Q12PartitionKey start_partition,
                                                           Q12PartitionKey end_partition,
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
	for (Q12PartitionKey key = start_partition; key < end_partition; ++key) {
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

void BuildQ12AuxState(duckdb::ClientContext &context) {
	const auto start = std::chrono::steady_clock::now();
	auto days = BuildAllDays(context);
	PublishDays(context, days);
	RecordQ12Rebuild();
	g_q12_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void RebuildQ12AuxPartition(duckdb::ClientContext &context, Q12PartitionKey partition) {
	const auto start = std::chrono::steady_clock::now();
	auto days = BuildOneDay(context, partition);
	auto &store = GetQ12Store(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	const auto publish_epoch = registry->GetMaintenanceState(kQ12AuxId).BumpCommittedMutationEpoch();
	auto it = days.find(partition);
	if (it == days.end()) {
		store.PublishPartition(partition, std::make_shared<Q12DayPartition>(), publish_epoch);
	} else {
		store.PublishPartition(partition, std::make_shared<Q12DayPartition>(it->second), publish_epoch);
	}
	RecordQ12Rebuild();
	g_q12_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void InvalidateQ12AuxStateForTests(duckdb::ClientContext &context) {
	GetQ12Store(context).InvalidateAll();
}

std::shared_ptr<const Q12DayPartition> TryGetReadableQ12DayPartition(duckdb::ClientContext &context,
                                                                    Q12PartitionKey partition) {
	EnsurePendingQ12RebuildApplied(context);
	auto &store = GetQ12Store(context);
	auto payload = store.GetReadablePayload(partition);
	if (!payload) {
		return nullptr;
	}
	return std::static_pointer_cast<const Q12DayPartition>(payload);
}

std::vector<Q12OutputRow> ComposeQ12AuxResult(duckdb::ClientContext &context, const std::string &shipmode1,
                                              const std::string &shipmode2, duckdb::date_t start_date) {
	const auto end_date = MakeQ12Parameters(shipmode1, shipmode2, start_date).end_date;
	const auto start_partition = PartitionKeyFromReceiptdate(start_date);
	const auto end_partition = PartitionKeyFromReceiptdate(end_date);
	auto &store = GetQ12Store(context);
	std::unordered_map<std::string, Q12MergedShipmodeStats> merged;
	int64_t partitions_merged = 0;
	store.VisitReadablePartitionsInRange(start_partition, end_partition,
	                                     [&](Q12PartitionKey key, const std::shared_ptr<const void> &payload) {
		                                     (void)key;
		                                     const auto &day = *std::static_pointer_cast<const Q12DayPartition>(payload);
		                                     partitions_merged++;
		                                     for (const auto &entry : day.shipmodes) {
			                                     if (entry.first != shipmode1 && entry.first != shipmode2) {
				                                     continue;
			                                     }
			                                     MergeQ12DayShipmodeStats(merged[entry.first], entry.second);
		                                     }
	                                     });
	g_q12_aux_diagnostics_mut.partitions_merged = partitions_merged;
	g_q12_aux_diagnostics_mut.start_partition = start_partition;
	g_q12_aux_diagnostics_mut.end_partition = end_partition;
	return ComposeQ12MergedOutput(merged, shipmode1, shipmode2);
}

} // namespace accelerator_codegen
