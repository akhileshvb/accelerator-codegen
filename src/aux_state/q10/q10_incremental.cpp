#include <stdexcept>

#include "accelerator_codegen/tpch_q10_incremental.hpp"
#include "accelerator_codegen/tpch_q10_write_policy.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/catalog_dependency.hpp"
#include "accelerator_codegen/chunk_source.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/tpch_q10_aux.hpp"
#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"
#include "accelerator_codegen/aux_state/maintenance/partition_publish.hpp"

#if ACCEL_HAS_POSTGRES_AUX
#include "accelerator_codegen/aux_state/postgres/postgres_aux_runtime.hpp"
#include "accelerator_codegen/aux_state/postgres/postgres_pg_lookup.hpp"
#endif

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint64_t kQ10MaxLightCaptureRows = 1024;
constexpr const char *kQ10AuxId = "tpch_q10_aux";

const aux_state::ColumnRef *FindColumn(const aux_state::TableDependency &dependency, const std::string &name) {
	for (const auto &column : dependency.watched_columns) {
		if (column.column_name == name) return &column;
	}
	return nullptr;
}

const aux_state::TableDependency *FindTableDependency(const std::vector<aux_state::TableDependency> &dependencies,
                                                      const std::string &table_name) {
	for (const auto &dependency : dependencies) {
		if (dependency.table_name == table_name) return &dependency;
	}
	return nullptr;
}

std::optional<int64_t> ReadInt64Value(const duckdb::Value &value) {
	if (value.IsNull()) {
		return std::nullopt;
	}
	if (value.type().id() == duckdb::LogicalTypeId::DECIMAL) {
		switch (value.type().InternalType()) {
		case duckdb::PhysicalType::INT16:
			return value.GetValueUnsafe<int16_t>();
		case duckdb::PhysicalType::INT32:
			return value.GetValueUnsafe<int32_t>();
		case duckdb::PhysicalType::INT64:
			return value.GetValueUnsafe<int64_t>();
		default:
			return std::nullopt;
		}
	}
	return value.GetValue<int64_t>();
}

std::vector<duckdb::Value> ValuesForColumn(const duckdb::TableMutationRow &row, duckdb::idx_t physical_index) {
	std::vector<duckdb::Value> values;
	for (const auto &projected : row.values) {
		if (projected.column_index.index == physical_index) values.push_back(projected.value);
	}
	return values;
}

std::optional<int64_t> PickInt64Column(const duckdb::TableMutationRow &row, const aux_state::ColumnRef &column,
                                       bool use_new_value) {
	auto values = ValuesForColumn(row, column.physical_index.index);
	if (values.empty()) return std::nullopt;
	return ReadInt64Value(values.size() == 1 || !use_new_value ? values.front() : values.back());
}

std::optional<std::string> PickStringColumn(const duckdb::TableMutationRow &row, const aux_state::ColumnRef &column,
                                            bool use_new_value) {
	auto values = ValuesForColumn(row, column.physical_index.index);
	if (values.empty()) return std::nullopt;
	const auto &picked = values.size() == 1 || !use_new_value ? values.front() : values.back();
	if (picked.IsNull()) return std::nullopt;
	return picked.GetValue<std::string>();
}

struct LineitemSnapshot {
	int64_t orderkey = 0;
	int64_t extendedprice_raw = 0;
	int64_t discount_raw = 0;
	std::string returnflag;
};

struct OrderSnapshot {
	int64_t custkey = 0;
	duckdb::date_t orderdate {};
};

struct Q10PendingLineitemOp {
	int8_t sign = 0;
	Q10PartitionKey partition = 0;
	int64_t custkey = 0;
	int64_t extendedprice_raw = 0;
	int64_t discount_raw = 0;
	bool has_net_revenue = false;
	Q10AccT net_revenue_raw = 0;
};

void NotePartition(aux_state::InvalidationPlan &plan, Q10PartitionKey partition) {
	if (partition == 0) return;
	plan.scope = aux_state::InvalidationScope::PartitionSet;
	plan.partitions.insert(partition);
}

std::optional<OrderSnapshot> LookupOrderSnapshot(duckdb::ClientContext &context, int64_t orderkey) {
#if ACCEL_HAS_POSTGRES_AUX
	if (auto *pg = aux_state::PostgresAuxRuntime::TryGetConnection(context)) {
		auto pg_snapshot = aux_state::LookupOrderSnapshotPostgres(*pg, orderkey);
		if (!pg_snapshot) {
			return std::nullopt;
		}
		OrderSnapshot snapshot;
		snapshot.custkey = pg_snapshot->custkey;
		snapshot.orderdate = pg_snapshot->orderdate;
		return snapshot;
	}
#endif
	NativeScanSpec order_spec;
	order_spec.table_name = "orders";
	order_spec.projected_columns = {
	    {"o_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_orderdate", "orderdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	};
	order_spec.filters = {{"o_orderkey", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value::BIGINT(orderkey)}};
	auto order_bind = BindNativeScan(context, order_spec);
	auto order_source = CreateNativeChunkSource(context, *order_bind);
	while (order_source->Next()) {
		auto &chunk = order_source->Chunk();
		duckdb::UnifiedVectorFormat custkeys;
		duckdb::UnifiedVectorFormat orderdates;
		chunk.data[1].ToUnifiedFormat(chunk.size(), custkeys);
		chunk.data[2].ToUnifiedFormat(chunk.size(), orderdates);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto ck_idx = custkeys.sel->get_index(row);
			const auto od_idx = orderdates.sel->get_index(row);
			if (!custkeys.validity.RowIsValid(ck_idx) || !orderdates.validity.RowIsValid(od_idx)) {
				return std::nullopt;
			}
			OrderSnapshot snapshot;
			snapshot.custkey = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkeys)[ck_idx];
			snapshot.orderdate = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(orderdates)[od_idx];
			return snapshot;
		}
	}
	return std::nullopt;
}

std::optional<OrderSnapshot> LookupOrderSnapshotCached(duckdb::ClientContext &context, int64_t orderkey,
                                                       std::unordered_map<int64_t, OrderSnapshot> &cache) {
	if (auto cached = cache.find(orderkey); cached != cache.end()) {
		return cached->second;
	}
	auto snapshot = LookupOrderSnapshot(context, orderkey);
	if (!snapshot) {
		return std::nullopt;
	}
	cache.emplace(orderkey, *snapshot);
	return snapshot;
}

std::optional<LineitemSnapshot> ReadLineitemSnapshot(const duckdb::TableMutationRow &row,
                                                     const aux_state::TableDependency &dependency, bool use_new_value) {
	const auto *orderkey_column = FindColumn(dependency, "l_orderkey");
	const auto *price_column = FindColumn(dependency, "l_extendedprice");
	const auto *discount_column = FindColumn(dependency, "l_discount");
	const auto *returnflag_column = FindColumn(dependency, "l_returnflag");
	if (!orderkey_column || !price_column || !discount_column || !returnflag_column) return std::nullopt;
	auto orderkey = PickInt64Column(row, *orderkey_column, use_new_value);
	auto price = PickInt64Column(row, *price_column, use_new_value);
	auto discount = PickInt64Column(row, *discount_column, use_new_value);
	auto returnflag = PickStringColumn(row, *returnflag_column, use_new_value);
	if (!orderkey || !price || !discount || !returnflag) return std::nullopt;
	LineitemSnapshot snapshot;
	snapshot.orderkey = *orderkey;
	snapshot.extendedprice_raw = *price;
	snapshot.discount_raw = *discount;
	snapshot.returnflag = *returnflag;
	return snapshot;
}

bool UpdateTouchesRelevantColumns(const duckdb::TableMutationEvent &event,
                                  const aux_state::TableDependency &dependency) {
	for (const auto &updated : event.updated_columns) {
		if (dependency.WatchesPhysicalIndex(updated.index)) return true;
	}
	return false;
}

std::unordered_map<Q10PartitionKey, Q10PartitionDelta> FoldLineitemOps(const std::vector<Q10PendingLineitemOp> &ops) {
	std::unordered_map<Q10PartitionKey, Q10PartitionDelta> folded;
	for (const auto &op : ops) {
		auto &net = folded[op.partition].customer_revenue_deltas[op.custkey];
		if (op.has_net_revenue) {
			Q10AddAccumulatedRevenue(net, op.net_revenue_raw, 1);
		} else {
			Q10AddRevenueRaw(net, op.extendedprice_raw, op.discount_raw, op.sign);
		}
	}
	return folded;
}

void ApplyDeltaToPartition(Q10MonthPartition &partition, const Q10PartitionDelta &delta) {
	for (const auto &entry : delta.customer_revenue_deltas) {
		Q10ApplyRevenueDeltaToPartition(partition, entry.first, entry.second);
	}
}

std::optional<Q10PendingLineitemOp> MakeOpFromSnapshot(duckdb::ClientContext &context, const LineitemSnapshot &snapshot,
                                                       int8_t sign,
                                                       std::unordered_map<int64_t, OrderSnapshot> &order_cache) {
	if (!Q10IsReturnedFlag(snapshot.returnflag)) {
		return std::nullopt;
	}
	auto order = LookupOrderSnapshotCached(context, snapshot.orderkey, order_cache);
	if (!order) {
		return std::nullopt;
	}
	Q10PendingLineitemOp op;
	op.sign = sign;
	op.partition = PartitionKeyFromOrderdate(order->orderdate);
	op.custkey = order->custkey;
	op.extendedprice_raw = snapshot.extendedprice_raw;
	op.discount_raw = snapshot.discount_raw;
	return op;
}

void ApplyCustomerDimMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                              const aux_state::TableDependency &dependency) {
	auto &stores = GetQ10SideStores(context);
	const auto *custkey_column = FindColumn(dependency, "c_custkey");
	const auto *name_column = FindColumn(dependency, "c_name");
	const auto *acctbal_column = FindColumn(dependency, "c_acctbal");
	const auto *nationkey_column = FindColumn(dependency, "c_nationkey");
	const auto *address_column = FindColumn(dependency, "c_address");
	const auto *phone_column = FindColumn(dependency, "c_phone");
	const auto *comment_column = FindColumn(dependency, "c_comment");
	if (!custkey_column) return;

	switch (event.kind) {
	case duckdb::TableMutationKind::DELETE:
		for (const auto &row : event.rows) {
			if (auto custkey = PickInt64Column(row, *custkey_column, false)) {
				stores.DeleteCustomer(*custkey);
			}
		}
		break;
	case duckdb::TableMutationKind::INSERT:
	case duckdb::TableMutationKind::UPDATE:
		for (const auto &row : event.rows) {
			auto custkey = PickInt64Column(row, *custkey_column, true);
			if (!custkey) continue;
			Q10CustomerDim dim;
			if (name_column) {
				if (auto name = PickStringColumn(row, *name_column, true)) dim.name = *name;
			}
			if (acctbal_column) {
				if (auto acctbal = PickInt64Column(row, *acctbal_column, true)) dim.acctbal_raw = *acctbal;
			}
			if (nationkey_column) {
				auto values = ValuesForColumn(row, nationkey_column->physical_index.index);
				if (!values.empty() && !values.back().IsNull()) {
					dim.nationkey = values.back().GetValue<int32_t>();
				}
			}
			if (address_column) {
				if (auto address = PickStringColumn(row, *address_column, true)) dim.address = *address;
			}
			if (phone_column) {
				if (auto phone = PickStringColumn(row, *phone_column, true)) dim.phone = *phone;
			}
			if (comment_column) {
				if (auto comment = PickStringColumn(row, *comment_column, true)) dim.comment = *comment;
			}
			stores.SetCustomer(*custkey, std::move(dim));
		}
		break;
	default:
		break;
	}
}

void ApplyNationDimMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                            const aux_state::TableDependency &dependency) {
	auto &stores = GetQ10SideStores(context);
	const auto *nationkey_column = FindColumn(dependency, "n_nationkey");
	const auto *name_column = FindColumn(dependency, "n_name");
	if (!nationkey_column || !name_column) return;
	for (const auto &row : event.rows) {
		auto nationkey_values = ValuesForColumn(row, nationkey_column->physical_index.index);
		auto name_values = ValuesForColumn(row, name_column->physical_index.index);
		if (nationkey_values.empty() || name_values.empty()) continue;
		const auto &nation_value = event.kind == duckdb::TableMutationKind::DELETE ? nationkey_values.front()
		                                                                              : nationkey_values.back();
		const auto &name_value = event.kind == duckdb::TableMutationKind::DELETE ? name_values.front() : name_values.back();
		if (nation_value.IsNull() || name_value.IsNull()) continue;
		const auto nationkey = nation_value.GetValue<int32_t>();
		if (nationkey < 0 || nationkey >= kQ10MaxNationSlots) continue;
		if (event.kind == duckdb::TableMutationKind::DELETE) {
			stores.nation_names[static_cast<size_t>(nationkey)].clear();
		} else {
			stores.nation_names[static_cast<size_t>(nationkey)] = name_value.GetValue<std::string>();
		}
	}
}

} // namespace

struct Q10IncrementalTxnBuffer::Impl {
	std::vector<Q10PendingLineitemOp> row_ops;
	aux_state::InvalidationPlan extra_partitions;
	std::unordered_map<int64_t, OrderSnapshot> order_snapshots;
	uint64_t cheap_rows_touched = 0;
	uint64_t estimated_lineitem_scan_rows = 0;
	uint64_t cheap_order_fanout = 0;
	uint64_t cross_partition_ops = 0;
	bool poisoned = false;
	bool heavy_capture_done = false;
	bool bulk_mutation = false;
};

Q10IncrementalTxnBuffer::Q10IncrementalTxnBuffer() : impl(std::make_unique<Impl>()) {}
Q10IncrementalTxnBuffer::~Q10IncrementalTxnBuffer() = default;
Q10IncrementalTxnBuffer::Q10IncrementalTxnBuffer(Q10IncrementalTxnBuffer &&) noexcept = default;
Q10IncrementalTxnBuffer &Q10IncrementalTxnBuffer::operator=(Q10IncrementalTxnBuffer &&) noexcept = default;

void Q10IncrementalTxnBuffer::Clear() {
	if (!impl) impl = std::make_unique<Impl>();
	impl->row_ops.clear();
	impl->extra_partitions = {};
	impl->order_snapshots.clear();
	impl->cheap_rows_touched = 0;
	impl->estimated_lineitem_scan_rows = 0;
	impl->cheap_order_fanout = 0;
	impl->cross_partition_ops = 0;
	impl->poisoned = false;
	impl->heavy_capture_done = false;
	impl->bulk_mutation = false;
}

bool Q10IncrementalTxnBuffer::HasPendingChanges() const {
	if (!impl) {
		return false;
	}
	return !impl->row_ops.empty() || impl->poisoned || impl->bulk_mutation ||
	       !impl->extra_partitions.IsEmpty();
}

uint64_t Q10IncrementalTxnBuffer::CheapRowsTouched() const {
	return impl ? impl->cheap_rows_touched : 0;
}
uint64_t Q10IncrementalTxnBuffer::CheapOrderFanout() const {
	return impl ? impl->cheap_order_fanout : 0;
}
uint64_t Q10IncrementalTxnBuffer::EstimatedLineitemScanRows() const {
	return impl ? impl->estimated_lineitem_scan_rows : 0;
}
bool Q10IncrementalTxnBuffer::HeavyCaptureDone() const {
	return impl && impl->heavy_capture_done;
}
bool Q10IncrementalTxnBuffer::BulkMutation() const {
	return impl && impl->bulk_mutation;
}
aux_state::InvalidationPlan Q10IncrementalTxnBuffer::ExtraInvalidationPlan() const {
	return impl ? impl->extra_partitions : aux_state::InvalidationPlan {};
}
bool Q10IncrementalTxnBuffer::IsPoisoned() const {
	return impl && impl->poisoned;
}
uint64_t Q10IncrementalTxnBuffer::RowOpCount() const {
	return impl ? impl->row_ops.size() : 0;
}
uint64_t Q10IncrementalTxnBuffer::CrossPartitionOpCount() const {
	return impl ? impl->cross_partition_ops : 0;
}
uint64_t Q10IncrementalTxnBuffer::PendingLightOpCount() const {
	return 0;
}

void MaybeMergePendingQ10LightOps(duckdb::ClientContext &context,
                                  const std::vector<aux_state::TableDependency> &dependencies,
                                  Q10IncrementalTxnBuffer &buffer) {
	(void)context;
	(void)dependencies;
	(void)buffer;
}

void Q10AccumulateLineitemMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                        const std::vector<aux_state::TableDependency> &dependencies,
                                        Q10IncrementalTxnBuffer &buffer) {
	if (!buffer.impl) buffer.impl = std::make_unique<Q10IncrementalTxnBuffer::Impl>();
	if (buffer.impl->poisoned) return;
	const auto *lineitem = FindTableDependency(dependencies, "lineitem");
	if (!lineitem || event.table_oid != lineitem->table_oid) return;

	auto touch = [&](size_t count) {
		buffer.impl->cheap_rows_touched += count;
		if (buffer.impl->cheap_rows_touched > kQ10MaxLightCaptureRows) buffer.impl->bulk_mutation = true;
	};

	auto append = [&](const LineitemSnapshot &snapshot, int8_t sign) {
		if (buffer.impl->bulk_mutation) return;
		if (auto op = MakeOpFromSnapshot(context, snapshot, sign, buffer.impl->order_snapshots)) {
			NotePartition(buffer.impl->extra_partitions, op->partition);
			buffer.impl->row_ops.push_back(*op);
		}
	};

	switch (event.kind) {
	case duckdb::TableMutationKind::INSERT:
		touch(event.rows.size());
		for (const auto &row : event.rows) {
			auto snapshot = ReadLineitemSnapshot(row, *lineitem, true);
			if (!snapshot) {
				buffer.impl->poisoned = true;
				return;
			}
			append(*snapshot, 1);
		}
		break;
	case duckdb::TableMutationKind::DELETE:
		touch(event.rows.size());
		for (const auto &row : event.rows) {
			auto snapshot = ReadLineitemSnapshot(row, *lineitem, false);
			if (!snapshot) {
				buffer.impl->poisoned = true;
				return;
			}
			append(*snapshot, -1);
		}
		break;
	case duckdb::TableMutationKind::UPDATE:
		if (!UpdateTouchesRelevantColumns(event, *lineitem)) return;
		for (const auto &row : event.rows) {
			auto old_snapshot = ReadLineitemSnapshot(row, *lineitem, false);
			auto new_snapshot = ReadLineitemSnapshot(row, *lineitem, true);
			if (!old_snapshot || !new_snapshot) {
				buffer.impl->poisoned = true;
				return;
			}
			const bool old_returned = Q10IsReturnedFlag(old_snapshot->returnflag);
			const bool new_returned = Q10IsReturnedFlag(new_snapshot->returnflag);
			if (!old_returned && !new_returned) {
				continue;
			}
			touch(1);
			if (old_returned && new_returned && old_snapshot->orderkey == new_snapshot->orderkey) {
				auto order = LookupOrderSnapshotCached(context, old_snapshot->orderkey, buffer.impl->order_snapshots);
				if (!order) {
					buffer.impl->poisoned = true;
					return;
				}
				const Q10AccT old_revenue =
				    Q10DiscountedRevenueRaw(old_snapshot->extendedprice_raw, old_snapshot->discount_raw);
				Q10AccT net_revenue =
				    Q10DiscountedRevenueRaw(new_snapshot->extendedprice_raw, new_snapshot->discount_raw);
				Q10AddAccumulatedRevenue(net_revenue, old_revenue, -1);
				if (net_revenue == 0) {
					continue;
				}
				Q10PendingLineitemOp op;
				op.partition = PartitionKeyFromOrderdate(order->orderdate);
				op.custkey = order->custkey;
				op.has_net_revenue = true;
				op.net_revenue_raw = net_revenue;
				NotePartition(buffer.impl->extra_partitions, op.partition);
				buffer.impl->row_ops.push_back(op);
				continue;
			}
			if (old_returned) {
				append(*old_snapshot, -1);
			}
			if (new_returned) {
				append(*new_snapshot, 1);
			}
		}
		break;
	default:
		return;
	}
}

void Q10AccumulateOrdersMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                      const std::vector<aux_state::TableDependency> &dependencies,
                                      Q10IncrementalTxnBuffer &buffer) {
	if (!buffer.impl) buffer.impl = std::make_unique<Q10IncrementalTxnBuffer::Impl>();
	const auto *orders = FindTableDependency(dependencies, "orders");
	if (!orders || event.table_oid != orders->table_oid) return;
	if (event.kind != duckdb::TableMutationKind::UPDATE || !UpdateTouchesRelevantColumns(event, *orders)) return;
	const auto *orderkey_column = FindColumn(*orders, "o_orderkey");
	const auto *custkey_column = FindColumn(*orders, "o_custkey");
	const auto *orderdate_column = FindColumn(*orders, "o_orderdate");
	if (!orderkey_column || !custkey_column || !orderdate_column) {
		buffer.impl->poisoned = true;
		return;
	}
	for (const auto &row : event.rows) {
		buffer.impl->cheap_order_fanout++;
		auto orderkey = PickInt64Column(row, *orderkey_column, true);
		if (!orderkey) continue;

		NativeScanSpec lineitem_spec;
		lineitem_spec.table_name = "lineitem";
		lineitem_spec.projected_columns = {
		    {"l_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
		    {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
		    {"l_discount", "discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
		    {"l_returnflag", "returnflag", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
		};
		lineitem_spec.filters = {
		    {"l_orderkey", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value::BIGINT(*orderkey)},
		    {"l_returnflag", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value("R")},
		};
		auto bind = BindNativeScan(context, lineitem_spec);
		auto source = CreateNativeChunkSource(context, *bind);

		auto old_custkey = PickInt64Column(row, *custkey_column, false);
		auto new_custkey = PickInt64Column(row, *custkey_column, true);
		auto old_orderdate_values = ValuesForColumn(row, orderdate_column->physical_index.index);
		auto new_orderdate_values = ValuesForColumn(row, orderdate_column->physical_index.index);
		duckdb::date_t old_orderdate {};
		duckdb::date_t new_orderdate {};
		bool has_old_orderdate = false;
		bool has_new_orderdate = false;
		if (!old_orderdate_values.empty() && !old_orderdate_values.front().IsNull()) {
			old_orderdate = old_orderdate_values.front().GetValue<duckdb::date_t>();
			has_old_orderdate = true;
		}
		if (!new_orderdate_values.empty() && !new_orderdate_values.back().IsNull()) {
			new_orderdate = new_orderdate_values.back().GetValue<duckdb::date_t>();
			has_new_orderdate = true;
		}
		if (!has_old_orderdate || !has_new_orderdate) {
			if (auto order = LookupOrderSnapshotCached(context, *orderkey, buffer.impl->order_snapshots)) {
				if (!has_old_orderdate) {
					old_orderdate = order->orderdate;
					has_old_orderdate = true;
				}
				if (!has_new_orderdate) {
					new_orderdate = order->orderdate;
					has_new_orderdate = true;
				}
				if (!old_custkey) old_custkey = order->custkey;
				if (!new_custkey) new_custkey = order->custkey;
			}
		}
		if (!old_custkey || !new_custkey || !has_old_orderdate || !has_new_orderdate) {
			buffer.impl->poisoned = true;
			return;
		}
		const auto old_partition = PartitionKeyFromOrderdate(old_orderdate);
		const auto new_partition = PartitionKeyFromOrderdate(new_orderdate);
		if (old_partition != new_partition) {
			buffer.impl->cross_partition_ops++;
		}

		while (source->Next()) {
			auto &chunk = source->Chunk();
			duckdb::UnifiedVectorFormat prices;
			duckdb::UnifiedVectorFormat discounts;
			chunk.data[1].ToUnifiedFormat(chunk.size(), prices);
			chunk.data[2].ToUnifiedFormat(chunk.size(), discounts);
			for (duckdb::idx_t r = 0; r < chunk.size(); r++) {
				const auto price =
				    duckdb::UnifiedVectorFormat::GetData<int64_t>(prices)[prices.sel->get_index(r)];
				const auto discount =
				    duckdb::UnifiedVectorFormat::GetData<int64_t>(discounts)[discounts.sel->get_index(r)];
				Q10PendingLineitemOp remove_op;
				remove_op.sign = -1;
				remove_op.partition = old_partition;
				remove_op.custkey = *old_custkey;
				remove_op.extendedprice_raw = price;
				remove_op.discount_raw = discount;
				Q10PendingLineitemOp add_op = remove_op;
				add_op.sign = 1;
				add_op.partition = new_partition;
				add_op.custkey = *new_custkey;
				NotePartition(buffer.impl->extra_partitions, old_partition);
				NotePartition(buffer.impl->extra_partitions, new_partition);
				buffer.impl->row_ops.push_back(remove_op);
				buffer.impl->row_ops.push_back(add_op);
			}
		}
	}
}

bool Q10FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                   const std::vector<aux_state::TableDependency> &dependencies,
                                   Q10IncrementalTxnBuffer &buffer) {
	(void)context;
	(void)dependencies;
	return buffer.impl && !buffer.impl->poisoned && !buffer.impl->row_ops.empty();
}

bool ComputeQ10IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                                const aux_state::InvalidationPlan &plan, Q10IncrementalTxnBuffer &buffer,
                                Q10ComputedIncrementalDelta &out) {
	(void)context;
	out = {};
	if (!buffer.impl || buffer.impl->poisoned) return false;
	if (plan.scope == aux_state::InvalidationScope::AllPartitions) return false;
	auto folded = FoldLineitemOps(buffer.impl->row_ops);
	std::unordered_set<Q10PartitionKey> partitions;
	if (plan.scope == aux_state::InvalidationScope::PartitionSet) {
		for (auto partition : plan.partitions) partitions.insert(partition);
	}
	for (const auto &entry : folded) partitions.insert(entry.first);
	for (auto partition : buffer.impl->extra_partitions.partitions) partitions.insert(partition);
	if (partitions.empty() && buffer.impl->row_ops.empty()) {
		out.success = true;
		return true;
	}
	for (auto partition : partitions) {
		const auto folded_it = folded.find(partition);
		if (folded_it == folded.end()) continue;
		if (store.GetStatus(partition) == aux_state::PartitionStatus::Invalid) return false;
		Q10MonthPartition partition_state;
		if (store.GetStatus(partition) == aux_state::PartitionStatus::Valid) {
			auto payload = store.GetReadablePayload(partition);
			if (!payload) return false;
			partition_state = *std::static_pointer_cast<const Q10MonthPartition>(payload);
		}
		ApplyDeltaToPartition(partition_state, folded_it->second);
		aux_state::maintenance::StagedPartitionUpdate update;
		update.key = partition;
		update.payload = std::make_shared<Q10MonthPartition>(std::move(partition_state));
		update.expected_partition_version = store.GetPartitionVersion(partition);
		out.partition_updates.push_back(std::move(update));
		out.affected_partitions.scope = aux_state::InvalidationScope::PartitionSet;
		out.affected_partitions.partitions.insert(partition);
	}
	out.success = true;
	RecordQ10IncrementalApplyAttempt(true, 0);
	return true;
}

void PublishQ10IncrementalDelta(aux_state::PartitionedStateStore &store, const Q10ComputedIncrementalDelta &delta,
                                uint64_t valid_through_mutation_epoch) {
	if (!delta.success) return;
	aux_state::maintenance::PublishValidatedPartitionUpdates(store, delta.partition_updates, valid_through_mutation_epoch);
}

Q10IncrementalTxnBuffer &GetQ10IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer) {
	auto &base = txn_buffer.GetOrCreateIncrementalBuffer(kQ10AuxId);
	auto *q10 = dynamic_cast<Q10IncrementalTxnBuffer *>(&base);
	if (!q10) throw std::runtime_error("incremental buffer for tpch_q10_aux has unexpected type");
	return *q10;
}

aux_state::InvalidationPlan AccumulateQ10Mutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                                  const std::vector<aux_state::TableDependency> &dependencies,
                                                  aux_state::TxnInvalidationBuffer &txn_buffer) {
	auto &incremental = GetQ10IncrementalTxnBuffer(txn_buffer);
	const bool dependent = IsQ10DependentTableMutation(event, dependencies);
	const auto *lineitem = FindTableDependency(dependencies, "lineitem");
	const auto *orders = FindTableDependency(dependencies, "orders");
	const auto *customer = FindTableDependency(dependencies, "customer");
	const auto *nation = FindTableDependency(dependencies, "nation");
	if (lineitem && event.table_oid == lineitem->table_oid) {
		Q10AccumulateLineitemMutationLight(context, event, dependencies, incremental);
	} else if (orders && event.table_oid == orders->table_oid) {
		Q10AccumulateOrdersMutationLight(context, event, dependencies, incremental);
	} else if (customer && event.table_oid == customer->table_oid) {
		ApplyCustomerDimMutation(context, event, *customer);
	} else if (nation && event.table_oid == nation->table_oid) {
		ApplyNationDimMutation(context, event, *nation);
	}
	if (dependent) {
		RecordQ10DependentTableMutation(incremental.ExtraInvalidationPlan().IsEmpty());
		RecordQ10WorkloadRelevantWrite();
	}
	return incremental.ExtraInvalidationPlan();
}

} // namespace accelerator_codegen
