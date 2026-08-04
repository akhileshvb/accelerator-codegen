#include "accelerator_codegen/tpch_q10_aux.hpp"
#include "accelerator_codegen/tpch_q10_rebuild_policy.hpp"
#include "accelerator_codegen/tpch_q10_write_policy.hpp"

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/chunk_source.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/tpch_q10.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/client_context.hpp"

#if ACCEL_HAS_POSTGRES_AUX
#include "accelerator_codegen/aux_state/postgres/postgres_aux_build.hpp"
#include "accelerator_codegen/aux_state/postgres/postgres_aux_runtime.hpp"
#include "accelerator_codegen/aux_state/postgres/postgres_pg_stream.hpp"
#include "libpq-fe.h"
#endif

namespace accelerator_codegen {
namespace {

constexpr const char *kQ10AuxId = "tpch_q10_aux";

struct OrderState {
	int64_t custkey = 0;
	duckdb::date_t orderdate {};
};

using MonthBuildMap = std::unordered_map<Q10PartitionKey, std::unordered_map<int64_t, Q10AccT>>;

std::unordered_map<duckdb::DatabaseInstance *, Q10SideStores> g_q10_side_stores;
std::mutex g_q10_side_stores_mutex;

NativeScanSpec CreateAuxCustomerScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "customer";
	scan_spec.projected_columns = {
	    {"c_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"c_name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"c_acctbal", "acctbal", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"c_nationkey", "nationkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	    {"c_address", "address", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"c_phone", "phone", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"c_comment", "comment", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

NativeScanSpec CreateAuxNationScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "nation";
	scan_spec.projected_columns = {
	    {"n_nationkey", "nationkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	    {"n_name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

NativeScanSpec CreateAuxOrdersScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "orders";
	scan_spec.projected_columns = {
	    {"o_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_orderdate", "orderdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
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
	    {"l_returnflag", "returnflag", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	scan_spec.filters = {
	    {"l_returnflag", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value("R")},
	};
	return scan_spec;
}

std::string ReadString(const duckdb::string_t &value) {
	return std::string(value.GetData(), value.GetData() + value.GetSize());
}

void ConsumeCustomerChunk(duckdb::DataChunk &chunk, Q10SideStores &stores) {
	duckdb::UnifiedVectorFormat custkeys;
	duckdb::UnifiedVectorFormat names;
	duckdb::UnifiedVectorFormat acctbals;
	duckdb::UnifiedVectorFormat nationkeys;
	duckdb::UnifiedVectorFormat addresses;
	duckdb::UnifiedVectorFormat phones;
	duckdb::UnifiedVectorFormat comments;
	chunk.data[0].ToUnifiedFormat(chunk.size(), custkeys);
	chunk.data[1].ToUnifiedFormat(chunk.size(), names);
	chunk.data[2].ToUnifiedFormat(chunk.size(), acctbals);
	chunk.data[3].ToUnifiedFormat(chunk.size(), nationkeys);
	chunk.data[4].ToUnifiedFormat(chunk.size(), addresses);
	chunk.data[5].ToUnifiedFormat(chunk.size(), phones);
	chunk.data[6].ToUnifiedFormat(chunk.size(), comments);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto ck_idx = custkeys.sel->get_index(row);
		const auto nm_idx = names.sel->get_index(row);
		const auto ab_idx = acctbals.sel->get_index(row);
		const auto nk_idx = nationkeys.sel->get_index(row);
		const auto ad_idx = addresses.sel->get_index(row);
		const auto ph_idx = phones.sel->get_index(row);
		const auto cm_idx = comments.sel->get_index(row);
		if (!custkeys.validity.RowIsValid(ck_idx) || !names.validity.RowIsValid(nm_idx) ||
		    !acctbals.validity.RowIsValid(ab_idx) || !nationkeys.validity.RowIsValid(nk_idx) ||
		    !addresses.validity.RowIsValid(ad_idx) || !phones.validity.RowIsValid(ph_idx) ||
		    !comments.validity.RowIsValid(cm_idx)) {
			continue;
		}
		const auto custkey = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkeys)[ck_idx];
		Q10CustomerDim dim;
		dim.name = ReadString(duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(names)[nm_idx]);
		dim.acctbal_raw = duckdb::UnifiedVectorFormat::GetData<int64_t>(acctbals)[ab_idx];
		dim.nationkey = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkeys)[nk_idx];
		dim.address = ReadString(duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(addresses)[ad_idx]);
		dim.phone = ReadString(duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(phones)[ph_idx]);
		dim.comment = ReadString(duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(comments)[cm_idx]);
		stores.SetCustomer(custkey, std::move(dim));
	}
}

void ConsumeNationChunk(duckdb::DataChunk &chunk, Q10SideStores &stores) {
	duckdb::UnifiedVectorFormat nationkeys;
	duckdb::UnifiedVectorFormat names;
	chunk.data[0].ToUnifiedFormat(chunk.size(), nationkeys);
	chunk.data[1].ToUnifiedFormat(chunk.size(), names);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto nk_idx = nationkeys.sel->get_index(row);
		const auto nm_idx = names.sel->get_index(row);
		if (!nationkeys.validity.RowIsValid(nk_idx) || !names.validity.RowIsValid(nm_idx)) {
			continue;
		}
		const auto nationkey = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkeys)[nk_idx];
		if (nationkey < 0 || nationkey >= kQ10MaxNationSlots) {
			continue;
		}
		stores.nation_names[static_cast<size_t>(nationkey)] =
		    ReadString(duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(names)[nm_idx]);
	}
}

void ConsumeOrdersChunk(duckdb::DataChunk &chunk, std::unordered_map<int64_t, OrderState> &orders) {
	duckdb::UnifiedVectorFormat orderkeys;
	duckdb::UnifiedVectorFormat custkeys;
	duckdb::UnifiedVectorFormat orderdates;
	chunk.data[0].ToUnifiedFormat(chunk.size(), orderkeys);
	chunk.data[1].ToUnifiedFormat(chunk.size(), custkeys);
	chunk.data[2].ToUnifiedFormat(chunk.size(), orderdates);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto ok_idx = orderkeys.sel->get_index(row);
		const auto ck_idx = custkeys.sel->get_index(row);
		const auto od_idx = orderdates.sel->get_index(row);
		if (!orderkeys.validity.RowIsValid(ok_idx) || !custkeys.validity.RowIsValid(ck_idx) ||
		    !orderdates.validity.RowIsValid(od_idx)) {
			continue;
		}
		OrderState state;
		state.custkey = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkeys)[ck_idx];
		state.orderdate = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(orderdates)[od_idx];
		orders[duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkeys)[ok_idx]] = std::move(state);
	}
}

void ConsumeLineitemChunk(duckdb::DataChunk &chunk, const std::unordered_map<int64_t, OrderState> &orders,
                          MonthBuildMap &months, const Q10PartitionKey *partition_filter) {
	duckdb::UnifiedVectorFormat orderkeys;
	duckdb::UnifiedVectorFormat prices;
	duckdb::UnifiedVectorFormat discounts;
	duckdb::UnifiedVectorFormat returnflags;
	chunk.data[0].ToUnifiedFormat(chunk.size(), orderkeys);
	chunk.data[1].ToUnifiedFormat(chunk.size(), prices);
	chunk.data[2].ToUnifiedFormat(chunk.size(), discounts);
	chunk.data[3].ToUnifiedFormat(chunk.size(), returnflags);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto ok_idx = orderkeys.sel->get_index(row);
		const auto pr_idx = prices.sel->get_index(row);
		const auto ds_idx = discounts.sel->get_index(row);
		const auto rf_idx = returnflags.sel->get_index(row);
		if (!orderkeys.validity.RowIsValid(ok_idx) || !prices.validity.RowIsValid(pr_idx) ||
		    !discounts.validity.RowIsValid(ds_idx)) {
			continue;
		}
		const auto &returnflag = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(returnflags)[rf_idx];
		if (!Q10IsReturnedLineitem(returnflag, !returnflags.validity.RowIsValid(rf_idx))) {
			continue;
		}
		const auto orderkey = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkeys)[ok_idx];
		const auto order_it = orders.find(orderkey);
		if (order_it == orders.end()) {
			continue;
		}
		const auto partition = PartitionKeyFromOrderdate(order_it->second.orderdate);
		if (partition_filter && partition != *partition_filter) {
			continue;
		}
		const auto price = duckdb::UnifiedVectorFormat::GetData<int64_t>(prices)[pr_idx];
		const auto discount = duckdb::UnifiedVectorFormat::GetData<int64_t>(discounts)[ds_idx];
		auto &revenue = months[partition][order_it->second.custkey];
		Q10AddRevenueRaw(revenue, price, discount, 1);
	}
}

void ScanCustomers(duckdb::ClientContext &context, Q10SideStores &stores) {
	auto bind_data = BindNativeScan(context, CreateAuxCustomerScanSpec());
	auto source = CreateNativeChunkSource(context, *bind_data);
	while (source->Next()) {
		ConsumeCustomerChunk(source->Chunk(), stores);
	}
}

void ScanNations(duckdb::ClientContext &context, Q10SideStores &stores) {
	auto bind_data = BindNativeScan(context, CreateAuxNationScanSpec());
	auto source = CreateNativeChunkSource(context, *bind_data);
	while (source->Next()) {
		ConsumeNationChunk(source->Chunk(), stores);
	}
}

void ScanOrders(duckdb::ClientContext &context, std::unordered_map<int64_t, OrderState> &orders) {
	auto bind_data = BindNativeScan(context, CreateAuxOrdersScanSpec());
	auto source = CreateNativeChunkSource(context, *bind_data);
	while (source->Next()) {
		ConsumeOrdersChunk(source->Chunk(), orders);
	}
}

void ScanLineitems(duckdb::ClientContext &context, const std::unordered_map<int64_t, OrderState> &orders,
                   MonthBuildMap &months, const Q10PartitionKey *partition_filter) {
	auto bind_data = BindNativeScan(context, CreateAuxLineitemScanSpec());
	auto source = CreateNativeChunkSource(context, *bind_data);
	while (source->Next()) {
		ConsumeLineitemChunk(source->Chunk(), orders, months, partition_filter);
	}
}

aux_state::PartitionedStateStore &GetQ10Store(duckdb::ClientContext &context) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	return registry->GetStore(kQ10AuxId);
}

void PublishMonths(duckdb::ClientContext &context, const MonthBuildMap &months) {
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	auto &store = GetQ10Store(context);
	const auto publish_epoch = registry->GetMaintenanceState(kQ10AuxId).BumpCommittedMutationEpoch();
	for (const auto &entry : months) {
		Q10MonthPartition partition;
		Q10BuildMonthPartitionFromScratch(partition, entry.second);
		store.PublishPartition(entry.first, std::make_shared<Q10MonthPartition>(std::move(partition)), publish_epoch);
	}
}

MonthBuildMap BuildAllMonths(duckdb::ClientContext &context) {
	std::unordered_map<int64_t, OrderState> orders;
	MonthBuildMap months;
	ScanOrders(context, orders);
	ScanLineitems(context, orders, months, nullptr);
	return months;
}

MonthBuildMap BuildOneMonth(duckdb::ClientContext &context, Q10PartitionKey partition) {
	std::unordered_map<int64_t, OrderState> orders;
	MonthBuildMap months;
	ScanOrders(context, orders);
	ScanLineitems(context, orders, months, &partition);
	return months;
}

#if ACCEL_HAS_POSTGRES_AUX
void BuildQ10SideStoresFromPostgres(PGconn &conn, duckdb::ClientContext &context) {
	auto &stores = GetQ10SideStores(context);
	stores.ClearCustomers();
	stores.ReserveCustomers();
	stores.nation_names.fill(std::string());

	aux_state::StreamPostgresQuery(conn,
	                    "SELECT c_custkey, c_name, c_acctbal, c_nationkey, c_address, c_phone, c_comment FROM customer",
	                    [&](PGresult *result, int row) {
		                    const int64_t custkey = std::stoll(PQgetvalue(result, row, 0));
		                    Q10CustomerDim dim;
		                    dim.name = PQgetvalue(result, row, 1);
		                    dim.acctbal_raw = aux_state::ParseDecimalScaled(PQgetvalue(result, row, 2), 2);
		                    dim.nationkey = std::stoi(PQgetvalue(result, row, 3));
		                    dim.address = PQgetvalue(result, row, 4);
		                    dim.phone = PQgetvalue(result, row, 5);
		                    dim.comment = PQgetvalue(result, row, 6);
		                    stores.SetCustomer(custkey, std::move(dim));
	                    });

	aux_state::StreamPostgresQuery(conn, "SELECT n_nationkey, n_name FROM nation", [&](PGresult *result, int row) {
		const auto nationkey = std::stoi(PQgetvalue(result, row, 0));
		if (nationkey < 0 || nationkey >= kQ10MaxNationSlots) {
			return;
		}
		stores.nation_names[static_cast<size_t>(nationkey)] = PQgetvalue(result, row, 1);
	});
}

MonthBuildMap BuildAllMonthsFromPostgres(PGconn &conn) {
	std::unordered_map<int64_t, OrderState> orders;
	MonthBuildMap months;

	aux_state::StreamPostgresQuery(conn, "SELECT o_orderkey, o_custkey, o_orderdate FROM orders", [&](PGresult *result, int row) {
		OrderState state;
		const auto orderkey = std::stoll(PQgetvalue(result, row, 0));
		state.custkey = std::stoll(PQgetvalue(result, row, 1));
		state.orderdate = duckdb::Date::FromString(PQgetvalue(result, row, 2));
		orders[orderkey] = std::move(state);
	});

	aux_state::StreamPostgresQuery(conn,
	                    "SELECT l_orderkey, l_extendedprice, l_discount, l_returnflag FROM lineitem WHERE l_returnflag = 'R'",
	                    [&](PGresult *result, int row) {
		                    const auto orderkey = std::stoll(PQgetvalue(result, row, 0));
		                    const auto order_it = orders.find(orderkey);
		                    if (order_it == orders.end()) {
			                    return;
		                    }
		                    const auto orderdate = order_it->second.orderdate;
		                    const auto partition =
		                        static_cast<Q10PartitionKey>(duckdb::Date::ExtractYear(orderdate) * 100 +
		                                                     duckdb::Date::ExtractMonth(orderdate));
		                    const auto price = aux_state::ParseDecimalScaled(PQgetvalue(result, row, 1), 2);
		                    const auto discount = aux_state::ParseDecimalScaled(PQgetvalue(result, row, 2), 2);
		                    auto &revenue = months[partition][order_it->second.custkey];
		                    Q10AddRevenueRaw(revenue, price, discount, 1);
	                    });
	return months;
}
#endif

} // namespace

namespace {
Q10AuxDiagnostics g_q10_aux_diagnostics_mut;
} // namespace

Q10SideStores &GetQ10SideStores(duckdb::ClientContext &context) {
	std::lock_guard<std::mutex> guard(g_q10_side_stores_mutex);
	return g_q10_side_stores[context.db.get()];
}

void BuildQ10SideStores(duckdb::ClientContext &context) {
	auto &stores = GetQ10SideStores(context);
	stores.ClearCustomers();
	stores.ReserveCustomers();
	stores.nation_names.fill(std::string());
	ScanCustomers(context, stores);
	ScanNations(context, stores);
}

void ResetQ10AuxDiagnostics() {
	g_q10_aux_diagnostics_mut = Q10AuxDiagnostics {};
}

void UpdateQ10AuxDiagnostics(const Q10AuxDiagnostics &diagnostics) {
	g_q10_aux_diagnostics_mut = diagnostics;
}

const Q10AuxDiagnostics &GetQ10AuxDiagnostics() {
	return g_q10_aux_diagnostics_mut;
}

Q10PartitionKey PartitionKeyFromOrderdate(duckdb::date_t orderdate) {
	const auto year = duckdb::Date::ExtractYear(orderdate);
	const auto month = duckdb::Date::ExtractMonth(orderdate);
	return static_cast<Q10PartitionKey>(year * 100 + month);
}

duckdb::date_t OrderdateFromPartitionKey(Q10PartitionKey key) {
	return Q10MonthStartFromPartitionKey(static_cast<int32_t>(key));
}

aux_state::ExecutionGateResult CheckQ10WindowExecutionGate(const aux_state::PartitionedStateStore &store,
                                                           const aux_state::TxnInvalidationBuffer &txn_buffer,
                                                           duckdb::date_t start_date, duckdb::date_t end_date,
                                                           uint64_t reader_visible_mutation_epoch) {
	aux_state::ExecutionGateResult result;
	if (txn_buffer.IsAllBypassed()) {
		result.bypassed_by_txn = true;
		result.committed_invalid = true;
		return result;
	}

	std::vector<int32_t> window_keys;
	Q10EnumerateWindowPartitionKeys(start_date, end_date, window_keys);
	if (window_keys.empty()) {
		result.committed_invalid = true;
		return result;
	}

	bool saw_any_partition = false;
	store.ForEachSlot([&](Q10PartitionKey key, aux_state::PartitionStatus status, const std::shared_ptr<const void> &,
	                      uint64_t) {
		saw_any_partition = true;
		(void)key;
		(void)status;
	});
	if (!saw_any_partition) {
		result.committed_invalid = true;
		return result;
	}

	for (const auto key : window_keys) {
		const auto partition_key = static_cast<Q10PartitionKey>(key);
		if (txn_buffer.IsPartitionBypassed(partition_key)) {
			result.bypassed_by_txn = true;
		}
		const auto status = store.GetStatus(partition_key);
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

void BuildQ10AuxState(duckdb::ClientContext &context) {
#if ACCEL_HAS_POSTGRES_AUX
	if (auto *pg = aux_state::PostgresAuxRuntime::TryGetConnection(context)) {
		aux_state::BuildQ10AuxStateFromPostgres(*pg, context);
		return;
	}
#endif
	const auto start = std::chrono::steady_clock::now();
	BuildQ10SideStores(context);
	auto months = BuildAllMonths(context);
	PublishMonths(context, months);
	RecordQ10Rebuild();
	g_q10_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void RebuildQ10AuxPartition(duckdb::ClientContext &context, Q10PartitionKey partition) {
	const auto start = std::chrono::steady_clock::now();
	auto months = BuildOneMonth(context, partition);
	auto &store = GetQ10Store(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	const auto publish_epoch = registry->GetMaintenanceState(kQ10AuxId).BumpCommittedMutationEpoch();
	auto it = months.find(partition);
	if (it == months.end() || it->second.empty()) {
		store.PublishPartition(partition, std::make_shared<Q10MonthPartition>(), publish_epoch);
	} else {
		Q10MonthPartition partition_state;
		Q10BuildMonthPartitionFromScratch(partition_state, it->second);
		store.PublishPartition(partition, std::make_shared<Q10MonthPartition>(std::move(partition_state)), publish_epoch);
	}
	RecordQ10Rebuild();
	g_q10_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void InvalidateQ10AuxStateForTests(duckdb::ClientContext &context) {
	GetQ10Store(context).InvalidateAll();
}

std::shared_ptr<const Q10MonthPartition> TryGetReadableQ10MonthPartition(duckdb::ClientContext &context,
                                                                         Q10PartitionKey partition) {
	EnsurePendingQ10RebuildApplied(context);
	auto &store = GetQ10Store(context);
	auto payload = store.GetReadablePayload(partition);
	if (!payload) {
		return nullptr;
	}
	return std::static_pointer_cast<const Q10MonthPartition>(payload);
}

std::vector<Q10RawSortKey> ComposeQ10AuxResult(duckdb::ClientContext &context, duckdb::date_t start_date) {
	EnsurePendingQ10RebuildApplied(context);
	const auto end_date = Q10ComputeExclusiveEndDate(start_date);
	auto &store = GetQ10Store(context);
	const auto &side_stores = GetQ10SideStores(context);

	static thread_local std::vector<Q10AccT> revenue_by_customer;
	static thread_local std::vector<int64_t> touched_customers;
	const auto scratch_size = static_cast<size_t>(kQ10TpchMaxCustkey) + 1;
	if (revenue_by_customer.size() < scratch_size) {
		revenue_by_customer.resize(scratch_size);
	}
	touched_customers.clear();

	std::vector<int32_t> window_partition_keys;
	Q10EnumerateWindowPartitionKeys(start_date, end_date, window_partition_keys);

	int64_t partitions_merged = 0;
	for (const auto partition_key : window_partition_keys) {
		const auto key = static_cast<Q10PartitionKey>(partition_key);
		auto payload = store.GetReadablePayload(key);
		if (!payload) {
			continue;
		}
		const auto &month = *std::static_pointer_cast<const Q10MonthPartition>(payload);
		partitions_merged++;
		for (const auto &entry : month.entries) {
			if (entry.revenue_raw == 0) {
				continue;
			}
			const auto custkey = entry.custkey;
			if (custkey <= 0 || custkey > kQ10TpchMaxCustkey) {
				continue;
			}
			const auto index = static_cast<size_t>(custkey);
			if (revenue_by_customer[index] == 0) {
				touched_customers.push_back(custkey);
			}
			Q10AddAccumulatedRevenue(revenue_by_customer[index], entry.revenue_raw, 1);
		}
	}

	g_q10_aux_diagnostics_mut.partitions_merged = partitions_merged;
	auto sort_keys = ComposeQ10SortedKeys(touched_customers, revenue_by_customer, side_stores);
	for (const auto custkey : touched_customers) {
		revenue_by_customer[static_cast<size_t>(custkey)] = 0;
	}
	return sort_keys;
}

#if ACCEL_HAS_POSTGRES_AUX
namespace aux_state {

void BuildQ10AuxStateFromPostgres(PGconn &conn, duckdb::ClientContext &context) {
	const auto start = std::chrono::steady_clock::now();
	BuildQ10SideStoresFromPostgres(conn, context);
	auto months = BuildAllMonthsFromPostgres(conn);
	PublishMonths(context, months);
	RecordQ10Rebuild();
	g_q10_aux_diagnostics_mut.build_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

} // namespace aux_state
#endif

} // namespace accelerator_codegen
