#include "accelerator_codegen/tpch_q14_aux.hpp"
#include "accelerator_codegen/tpch_q14_rebuild_policy.hpp"
#include "accelerator_codegen/tpch_q14_write_policy.hpp"

#include <chrono>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/chunk_source.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/tpch_q14.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

constexpr const char *kQ14AuxId = "tpch_q14_aux";
constexpr size_t kDefaultPartCapacity = 200001;

enum class PromoClass : uint8_t { Absent = 0, NonPromo = 1, Promo = 2 };

bool IsPromoType(const duckdb::string_t &value) {
	static constexpr const char kPromo[] = "PROMO";
	constexpr auto kPromoSize = sizeof(kPromo) - 1;
	return value.GetSize() >= kPromoSize && std::memcmp(value.GetData(), kPromo, kPromoSize) == 0;
}

Q14AccT DiscountedRevenueRaw(int64_t extendedprice_raw, int64_t discount_raw) {
	return static_cast<Q14AccT>(extendedprice_raw) * static_cast<Q14AccT>(100 - discount_raw);
}

class PartPromoStore {
public:
	PromoClass Probe(int64_t partkey) const {
		if (partkey < 0) {
			const auto it = overflow.find(partkey);
			return it == overflow.end() ? PromoClass::Absent : it->second;
		}
		const auto key = static_cast<size_t>(partkey);
		if (key < classes.size()) {
			return static_cast<PromoClass>(classes[key]);
		}
		const auto it = overflow.find(partkey);
		return it == overflow.end() ? PromoClass::Absent : it->second;
	}

	void Store(int64_t partkey, PromoClass promo) {
		if (partkey < 0 || static_cast<size_t>(partkey) >= classes.size()) {
			overflow[partkey] = promo;
			return;
		}
		classes[static_cast<size_t>(partkey)] = static_cast<uint8_t>(promo);
	}

	void EnsureCapacity(size_t size) {
		if (classes.size() < size) {
			classes.resize(size, static_cast<uint8_t>(PromoClass::Absent));
		}
	}

private:
	std::vector<uint8_t> classes = std::vector<uint8_t>(kDefaultPartCapacity, static_cast<uint8_t>(PromoClass::Absent));
	std::unordered_map<int64_t, PromoClass> overflow;
};

struct BuildContext {
	PartPromoStore part_promo;
	std::unordered_map<Q14PartitionKey, Q14MonthTotals> months;
};

void ConsumePartChunk(duckdb::DataChunk &chunk, PartPromoStore &store) {
	duckdb::UnifiedVectorFormat partkeys;
	duckdb::UnifiedVectorFormat types;
	chunk.data[0].ToUnifiedFormat(chunk.size(), partkeys);
	chunk.data[1].ToUnifiedFormat(chunk.size(), types);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto partkey_idx = partkeys.sel->get_index(row);
		if (!partkeys.validity.RowIsValid(partkey_idx)) {
			continue;
		}
		const auto type_idx = types.sel->get_index(row);
		if (!types.validity.RowIsValid(type_idx)) {
			store.Store(duckdb::UnifiedVectorFormat::GetData<int64_t>(partkeys)[partkey_idx], PromoClass::NonPromo);
			continue;
		}
		const auto partkey = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkeys)[partkey_idx];
		const auto &type = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(types)[type_idx];
		store.Store(partkey, IsPromoType(type) ? PromoClass::Promo : PromoClass::NonPromo);
	}
}

void ConsumeLineitemChunk(duckdb::DataChunk &chunk, BuildContext &context) {
	duckdb::UnifiedVectorFormat partkeys;
	duckdb::UnifiedVectorFormat prices;
	duckdb::UnifiedVectorFormat discounts;
	duckdb::UnifiedVectorFormat shipdates;
	chunk.data[0].ToUnifiedFormat(chunk.size(), partkeys);
	chunk.data[1].ToUnifiedFormat(chunk.size(), prices);
	chunk.data[2].ToUnifiedFormat(chunk.size(), discounts);
	chunk.data[3].ToUnifiedFormat(chunk.size(), shipdates);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto partkey_idx = partkeys.sel->get_index(row);
		const auto price_idx = prices.sel->get_index(row);
		const auto discount_idx = discounts.sel->get_index(row);
		const auto shipdate_idx = shipdates.sel->get_index(row);
		if (!partkeys.validity.RowIsValid(partkey_idx) || !prices.validity.RowIsValid(price_idx) ||
		    !discounts.validity.RowIsValid(discount_idx) || !shipdates.validity.RowIsValid(shipdate_idx)) {
			continue;
		}
		const auto partkey = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkeys)[partkey_idx];
		const auto price = duckdb::UnifiedVectorFormat::GetData<int64_t>(prices)[price_idx];
		const auto discount = duckdb::UnifiedVectorFormat::GetData<int64_t>(discounts)[discount_idx];
		const auto shipdate = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdates)[shipdate_idx];
		if (context.part_promo.Probe(partkey) == PromoClass::Absent) {
			continue;
		}
		const auto month = PartitionKeyFromDate(shipdate);
		auto &totals = context.months[month];
		const auto revenue = DiscountedRevenueRaw(price, discount);
		totals.total_revenue_raw += revenue;
		totals.row_count++;
		totals.saw_contribution = true;
		if (context.part_promo.Probe(partkey) == PromoClass::Promo) {
			totals.promo_revenue_raw += revenue;
		}
	}
}

NativeScanSpec CreateAuxPartScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "part";
	scan_spec.projected_columns = {
	    {"p_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"p_type", "type", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

NativeScanSpec CreateAuxLineitemScanSpec(const Q14Parameters *parameters = nullptr) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_discount", "discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	};
	if (parameters) {
		scan_spec.filters = {
		    {"l_shipdate", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
		     duckdb::Value::DATE(parameters->start_date)},
		    {"l_shipdate", duckdb::ExpressionType::COMPARE_LESSTHAN, duckdb::Value::DATE(parameters->end_date)},
		};
	}
	return scan_spec;
}

void ScanPart(duckdb::ClientContext &context, PartPromoStore &store) {
	auto bind_data = BindNativeScan(context, CreateAuxPartScanSpec());
	auto source = CreateNativeChunkSource(context, *bind_data);
	while (source->Next()) {
		ConsumePartChunk(source->Chunk(), store);
	}
}

void ScanLineitem(duckdb::ClientContext &context, BuildContext &build_context, const Q14Parameters *month_filter) {
	auto bind_data = BindNativeScan(context, month_filter ? CreateAuxLineitemScanSpec(month_filter) : CreateAuxLineitemScanSpec());
	auto source = CreateNativeChunkSource(context, *bind_data);
	while (source->Next()) {
		ConsumeLineitemChunk(source->Chunk(), build_context);
	}
}

duckdb::date_t PartitionKeyToStartDate(Q14PartitionKey key) {
	const auto year = key / 100;
	const auto month = key % 100;
	return duckdb::Date::FromDate(year, month, 1);
}

aux_state::PartitionedStateStore &GetQ14Store(duckdb::ClientContext &context) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	return registry->GetStore(kQ14AuxId);
}

void PublishMonths(duckdb::ClientContext &context, const std::unordered_map<Q14PartitionKey, Q14MonthTotals> &months) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	auto &store = registry->GetStore(kQ14AuxId);
	const auto publish_epoch = registry->GetMaintenanceState(kQ14AuxId).BumpCommittedMutationEpoch();
	for (const auto &entry : months) {
		store.PublishPartition(entry.first, std::make_shared<Q14MonthTotals>(entry.second), publish_epoch);
	}
}

} // namespace

namespace {
Q14AuxDiagnostics g_q14_aux_diagnostics_mut;
} // namespace

void ResetQ14AuxDiagnostics() {
	g_q14_aux_diagnostics_mut = Q14AuxDiagnostics {};
}

void UpdateQ14AuxDiagnostics(const Q14AuxDiagnostics &diagnostics) {
	g_q14_aux_diagnostics_mut = diagnostics;
}

const Q14AuxDiagnostics &GetQ14AuxDiagnostics() {
	return g_q14_aux_diagnostics_mut;
}

void BuildQ14AuxState(duckdb::ClientContext &context) {
	const auto start = std::chrono::steady_clock::now();
	BuildContext build_context;
	ScanPart(context, build_context.part_promo);
	ScanLineitem(context, build_context, nullptr);
	PublishMonths(context, build_context.months);
	RecordQ14Rebuild();
	g_q14_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void RebuildQ14AuxPartition(duckdb::ClientContext &context, Q14PartitionKey partition) {
	const auto start = std::chrono::steady_clock::now();
	BuildContext build_context;
	ScanPart(context, build_context.part_promo);
	Q14Parameters parameters;
	parameters.start_date = PartitionKeyToStartDate(partition);
	parameters.end_date = MakeQ14Parameters(parameters.start_date).end_date;
	ScanLineitem(context, build_context, &parameters);
	auto &store = GetQ14Store(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	const auto publish_epoch = registry->GetMaintenanceState(kQ14AuxId).BumpCommittedMutationEpoch();
	auto it = build_context.months.find(partition);
	if (it == build_context.months.end()) {
		Q14MonthTotals empty;
		store.PublishPartition(partition, std::make_shared<Q14MonthTotals>(empty), publish_epoch);
	} else {
		store.PublishPartition(partition, std::make_shared<Q14MonthTotals>(it->second), publish_epoch);
	}
	RecordQ14Rebuild();
	g_q14_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void InvalidateQ14AuxStateForTests(duckdb::ClientContext &context) {
	GetQ14Store(context).InvalidateAll();
}

std::shared_ptr<const Q14MonthTotals> TryGetReadableMonthTotals(duckdb::ClientContext &context,
                                                                Q14PartitionKey partition) {
	EnsurePendingQ14RebuildApplied(context);
	auto &store = GetQ14Store(context);
	auto payload = store.GetReadablePayload(partition);
	if (!payload) {
		return nullptr;
	}
	return std::static_pointer_cast<const Q14MonthTotals>(payload);
}

} // namespace accelerator_codegen
