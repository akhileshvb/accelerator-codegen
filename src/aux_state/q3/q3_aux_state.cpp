#include "accelerator_codegen/tpch_q3_aux.hpp"
#include "accelerator_codegen/tpch_q3_rebuild_policy.hpp"
#include "accelerator_codegen/tpch_q3_write_policy.hpp"

#include <chrono>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/chunk_source.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/tpch_q3.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

constexpr const char *kQ3AuxId = "tpch_q3_aux";

using CustSegmentMap = std::unordered_map<int64_t, Q3PartitionKey>;
using SegmentBuildMap = std::unordered_map<Q3PartitionKey, Q3SegmentPartition>;

NativeScanSpec CreateAuxCustomerScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "customer";
	scan_spec.projected_columns = {
	    {"c_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"c_mktsegment", "mktsegment", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

NativeScanSpec CreateAuxOrdersScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "orders";
	scan_spec.projected_columns = {
	    {"o_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_orderdate", "orderdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	    {"o_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_shippriority", "shippriority", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	return scan_spec;
}

NativeScanSpec CreateAuxLineitemScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_discount", "discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	};
	return scan_spec;
}

Q3PartitionKey SegmentFromString(const duckdb::string_t &segment) {
	static constexpr const char *kSegments[] = {"BUILDING", "FURNITURE", "HOUSEHOLD", "MACHINERY", "AUTOMOBILE"};
	for (size_t i = 0; i < 5; i++) {
		const auto len = std::strlen(kSegments[i]);
		if (segment.GetSize() == len && std::memcmp(segment.GetData(), kSegments[i], len) == 0) {
			return static_cast<Q3PartitionKey>(i + 1);
		}
	}
	return 0;
}

void ConsumeCustomerChunk(duckdb::DataChunk &chunk, CustSegmentMap &cust_segments, const Q3PartitionKey *segment_filter) {
	duckdb::UnifiedVectorFormat custkeys;
	duckdb::UnifiedVectorFormat segments;
	chunk.data[0].ToUnifiedFormat(chunk.size(), custkeys);
	chunk.data[1].ToUnifiedFormat(chunk.size(), segments);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto cust_idx = custkeys.sel->get_index(row);
		const auto seg_idx = segments.sel->get_index(row);
		if (!custkeys.validity.RowIsValid(cust_idx) || !segments.validity.RowIsValid(seg_idx)) {
			continue;
		}
		const auto custkey = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkeys)[cust_idx];
		const auto &segment = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(segments)[seg_idx];
		const auto partition = SegmentFromString(segment);
		if (partition == 0) {
			continue;
		}
		if (segment_filter && partition != *segment_filter) {
			continue;
		}
		cust_segments[custkey] = partition;
	}
}

void ConsumeOrdersChunk(duckdb::DataChunk &chunk, const CustSegmentMap &cust_segments, SegmentBuildMap &segments) {
	duckdb::UnifiedVectorFormat orderkeys;
	duckdb::UnifiedVectorFormat orderdates;
	duckdb::UnifiedVectorFormat custkeys;
	duckdb::UnifiedVectorFormat shippriorities;
	chunk.data[0].ToUnifiedFormat(chunk.size(), orderkeys);
	chunk.data[1].ToUnifiedFormat(chunk.size(), orderdates);
	chunk.data[2].ToUnifiedFormat(chunk.size(), custkeys);
	chunk.data[3].ToUnifiedFormat(chunk.size(), shippriorities);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto ok_idx = orderkeys.sel->get_index(row);
		const auto od_idx = orderdates.sel->get_index(row);
		const auto ck_idx = custkeys.sel->get_index(row);
		const auto sp_idx = shippriorities.sel->get_index(row);
		if (!orderkeys.validity.RowIsValid(ok_idx) || !orderdates.validity.RowIsValid(od_idx) ||
		    !custkeys.validity.RowIsValid(ck_idx) || !shippriorities.validity.RowIsValid(sp_idx)) {
			continue;
		}
		const auto orderkey = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkeys)[ok_idx];
		const auto custkey = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkeys)[ck_idx];
		const auto it = cust_segments.find(custkey);
		if (it == cust_segments.end()) {
			continue;
		}
		Q3OrderState state;
		state.o_orderdate = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(orderdates)[od_idx];
		state.o_custkey = custkey;
		state.o_shippriority = duckdb::UnifiedVectorFormat::GetData<int32_t>(shippriorities)[sp_idx];
		segments[it->second].orders[orderkey] = std::move(state);
	}
}

void ConsumeLineitemChunk(duckdb::DataChunk &chunk, SegmentBuildMap &segments) {
	duckdb::UnifiedVectorFormat orderkeys;
	duckdb::UnifiedVectorFormat prices;
	duckdb::UnifiedVectorFormat discounts;
	duckdb::UnifiedVectorFormat shipdates;
	chunk.data[0].ToUnifiedFormat(chunk.size(), orderkeys);
	chunk.data[1].ToUnifiedFormat(chunk.size(), prices);
	chunk.data[2].ToUnifiedFormat(chunk.size(), discounts);
	chunk.data[3].ToUnifiedFormat(chunk.size(), shipdates);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto ok_idx = orderkeys.sel->get_index(row);
		const auto price_idx = prices.sel->get_index(row);
		const auto discount_idx = discounts.sel->get_index(row);
		const auto ship_idx = shipdates.sel->get_index(row);
		if (!orderkeys.validity.RowIsValid(ok_idx) || !prices.validity.RowIsValid(price_idx) ||
		    !discounts.validity.RowIsValid(discount_idx) || !shipdates.validity.RowIsValid(ship_idx)) {
			continue;
		}
		const auto orderkey = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkeys)[ok_idx];
		const auto price = duckdb::UnifiedVectorFormat::GetData<int64_t>(prices)[price_idx];
		const auto discount = duckdb::UnifiedVectorFormat::GetData<int64_t>(discounts)[discount_idx];
		const auto shipdate = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdates)[ship_idx];
		for (auto &segment_entry : segments) {
			auto order_it = segment_entry.second.orders.find(orderkey);
			if (order_it == segment_entry.second.orders.end()) {
				continue;
			}
			AddBucketRevenue(order_it->second.shipdate_buckets[shipdate.days], price, discount, 1);
			break;
		}
	}
}

void ScanCustomers(duckdb::ClientContext &context, CustSegmentMap &cust_segments, const Q3PartitionKey *segment_filter) {
	auto bind_data = BindNativeScan(context, CreateAuxCustomerScanSpec());
	auto source = CreateNativeChunkSource(context, *bind_data);
	while (source->Next()) {
		ConsumeCustomerChunk(source->Chunk(), cust_segments, segment_filter);
	}
}

void ScanOrders(duckdb::ClientContext &context, const CustSegmentMap &cust_segments, SegmentBuildMap &segments) {
	auto bind_data = BindNativeScan(context, CreateAuxOrdersScanSpec());
	auto source = CreateNativeChunkSource(context, *bind_data);
	while (source->Next()) {
		ConsumeOrdersChunk(source->Chunk(), cust_segments, segments);
	}
}

void ScanLineitems(duckdb::ClientContext &context, SegmentBuildMap &segments) {
	auto bind_data = BindNativeScan(context, CreateAuxLineitemScanSpec());
	auto source = CreateNativeChunkSource(context, *bind_data);
	while (source->Next()) {
		ConsumeLineitemChunk(source->Chunk(), segments);
	}
}

aux_state::PartitionedStateStore &GetQ3Store(duckdb::ClientContext &context) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	return registry->GetStore(kQ3AuxId);
}

void PublishSegments(duckdb::ClientContext &context, const SegmentBuildMap &segments) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	auto &store = registry->GetStore(kQ3AuxId);
	const auto publish_epoch = registry->GetMaintenanceState(kQ3AuxId).BumpCommittedMutationEpoch();
	for (const auto &entry : segments) {
		store.PublishPartition(entry.first, std::make_shared<Q3SegmentPartition>(entry.second), publish_epoch);
	}
}

SegmentBuildMap BuildAllSegments(duckdb::ClientContext &context) {
	CustSegmentMap cust_segments;
	SegmentBuildMap segments;
	ScanCustomers(context, cust_segments, nullptr);
	ScanOrders(context, cust_segments, segments);
	ScanLineitems(context, segments);
	return segments;
}

SegmentBuildMap BuildOneSegment(duckdb::ClientContext &context, Q3PartitionKey partition) {
	CustSegmentMap cust_segments;
	SegmentBuildMap segments;
	ScanCustomers(context, cust_segments, &partition);
	ScanOrders(context, cust_segments, segments);
	ScanLineitems(context, segments);
	return segments;
}

} // namespace

namespace {
Q3AuxDiagnostics g_q3_aux_diagnostics_mut;
} // namespace

void ResetQ3AuxDiagnostics() {
	g_q3_aux_diagnostics_mut = Q3AuxDiagnostics {};
}

void UpdateQ3AuxDiagnostics(const Q3AuxDiagnostics &diagnostics) {
	g_q3_aux_diagnostics_mut = diagnostics;
}

const Q3AuxDiagnostics &GetQ3AuxDiagnostics() {
	return g_q3_aux_diagnostics_mut;
}

Q3PartitionKey PartitionKeyFromMktsegment(const std::string &segment) {
	if (segment == "BUILDING") {
		return kQ3SegmentBuilding;
	}
	if (segment == "FURNITURE") {
		return kQ3SegmentFurniture;
	}
	if (segment == "HOUSEHOLD") {
		return kQ3SegmentHousehold;
	}
	if (segment == "MACHINERY") {
		return kQ3SegmentMachinery;
	}
	if (segment == "AUTOMOBILE") {
		return kQ3SegmentAutomobile;
	}
	return 0;
}

std::string MktsegmentFromPartitionKey(Q3PartitionKey key) {
	switch (key) {
	case kQ3SegmentBuilding:
		return "BUILDING";
	case kQ3SegmentFurniture:
		return "FURNITURE";
	case kQ3SegmentHousehold:
		return "HOUSEHOLD";
	case kQ3SegmentMachinery:
		return "MACHINERY";
	case kQ3SegmentAutomobile:
		return "AUTOMOBILE";
	default:
		return {};
	}
}

void BuildQ3AuxState(duckdb::ClientContext &context) {
	const auto start = std::chrono::steady_clock::now();
	auto segments = BuildAllSegments(context);
	PublishSegments(context, segments);
	RecordQ3Rebuild();
	g_q3_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void RebuildQ3AuxPartition(duckdb::ClientContext &context, Q3PartitionKey partition) {
	const auto start = std::chrono::steady_clock::now();
	auto segments = BuildOneSegment(context, partition);
	auto &store = GetQ3Store(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	const auto publish_epoch = registry->GetMaintenanceState(kQ3AuxId).BumpCommittedMutationEpoch();
	auto it = segments.find(partition);
	if (it == segments.end()) {
		store.PublishPartition(partition, std::make_shared<Q3SegmentPartition>(), publish_epoch);
	} else {
		store.PublishPartition(partition, std::make_shared<Q3SegmentPartition>(it->second), publish_epoch);
	}
	RecordQ3Rebuild();
	g_q3_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void InvalidateQ3AuxStateForTests(duckdb::ClientContext &context) {
	GetQ3Store(context).InvalidateAll();
}

std::shared_ptr<const Q3SegmentPartition> TryGetReadableSegmentPartition(duckdb::ClientContext &context,
                                                                         Q3PartitionKey partition) {
	EnsurePendingQ3RebuildApplied(context);
	auto &store = GetQ3Store(context);
	auto payload = store.GetReadablePayload(partition);
	if (!payload) {
		return nullptr;
	}
	return std::static_pointer_cast<const Q3SegmentPartition>(payload);
}

std::vector<Q3OutputRow> ComposeQ3AuxResult(duckdb::ClientContext &context, const std::string &market_segment,
                                            duckdb::date_t cutoff_date) {
	const auto partition = PartitionKeyFromMktsegment(market_segment);
	auto segment = TryGetReadableSegmentPartition(context, partition);
	if (!segment) {
		return {};
	}
	g_q3_aux_diagnostics_mut.orders_scanned = static_cast<int64_t>(segment->orders.size());
	return ComposeQ3TopK(*segment, cutoff_date);
}

} // namespace accelerator_codegen
