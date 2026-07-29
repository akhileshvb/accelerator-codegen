#include <stdexcept>

#include "accelerator_codegen/tpch_q14_incremental.hpp"
#include "accelerator_codegen/tpch_q14_write_policy.hpp"
#include "accelerator_codegen/tpch_q14_maintenance.hpp"

#include <chrono>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "accelerator_codegen/aux_state/catalog_dependency.hpp"
#include "accelerator_codegen/chunk_source.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/tpch_q14_aux.hpp"
#include "accelerator_codegen/aux_state/maintenance/staged_publication.hpp"
#include "accelerator_codegen/aux_state/maintenance/partition_publish.hpp"
#include "accelerator_codegen/tpch_q14_write_policy.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

constexpr const char *kQ14AuxId = "tpch_q14_aux";

bool IsPromoType(const duckdb::string_t &value) {
	static constexpr const char kPromo[] = "PROMO";
	constexpr auto kPromoSize = sizeof(kPromo) - 1;
	return value.GetSize() >= kPromoSize && std::memcmp(value.GetData(), kPromo, kPromoSize) == 0;
}

bool IsPromoValue(const duckdb::Value &value) {
	if (value.IsNull()) {
		return false;
	}
	const auto type = value.GetValue<std::string>();
	static constexpr const char kPromo[] = "PROMO";
	constexpr auto kPromoSize = sizeof(kPromo) - 1;
	return type.size() >= kPromoSize && type.compare(0, kPromoSize, kPromo) == 0;
}

Q14AccT DiscountedRevenueRaw(int64_t extendedprice_raw, int64_t discount_raw) {
	return static_cast<Q14AccT>(extendedprice_raw) * static_cast<Q14AccT>(100 - discount_raw);
}

const aux_state::ColumnRef *FindColumn(const aux_state::TableDependency &dependency, const std::string &name) {
	for (const auto &column : dependency.watched_columns) {
		if (column.column_name == name) {
			return &column;
		}
	}
	return nullptr;
}

const aux_state::TableDependency *FindTableDependency(const std::vector<aux_state::TableDependency> &dependencies,
                                                      const std::string &table_name) {
	for (const auto &dependency : dependencies) {
		if (dependency.table_name == table_name) {
			return &dependency;
		}
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

std::optional<duckdb::date_t> ReadDateValue(const duckdb::Value &value) {
	if (value.IsNull()) {
		return std::nullopt;
	}
	return value.GetValue<duckdb::date_t>();
}

std::vector<duckdb::Value> ValuesForColumn(const duckdb::TableMutationRow &row, duckdb::idx_t physical_index) {
	std::vector<duckdb::Value> values;
	for (const auto &projected : row.values) {
		if (projected.column_index.index == physical_index) {
			values.push_back(projected.value);
		}
	}
	return values;
}

std::optional<int64_t> PickInt64Column(const duckdb::TableMutationRow &row, const aux_state::ColumnRef &column,
                                       bool use_new_value) {
	auto values = ValuesForColumn(row, column.physical_index.index);
	if (values.empty()) {
		return std::nullopt;
	}
	if (values.size() == 1 || !use_new_value) {
		return ReadInt64Value(values.front());
	}
	return ReadInt64Value(values.back());
}

std::optional<duckdb::date_t> PickDateColumn(const duckdb::TableMutationRow &row, const aux_state::ColumnRef &column,
                                             bool use_new_value) {
	auto values = ValuesForColumn(row, column.physical_index.index);
	if (values.empty()) {
		return std::nullopt;
	}
	if (values.size() == 1 || !use_new_value) {
		return ReadDateValue(values.front());
	}
	return ReadDateValue(values.back());
}

std::optional<duckdb::Value> PickValueColumn(const duckdb::TableMutationRow &row, const aux_state::ColumnRef &column,
                                             bool use_new_value) {
	auto values = ValuesForColumn(row, column.physical_index.index);
	if (values.empty()) {
		return std::nullopt;
	}
	if (values.size() == 1 || !use_new_value) {
		return values.front();
	}
	return values.back();
}

struct RowSnapshot {
	Q14PartitionKey month = 0;
	int64_t partkey = 0;
	int64_t price = 0;
	int64_t discount = 0;
};

struct Q14PendingRowOp {
	int8_t sign = 0;
	Q14PartitionKey month = 0;
	int64_t partkey = 0;
	int64_t price = 0;
	int64_t discount = 0;
	bool is_promo = false;
};

struct Q14PendingPromoRevenueDelta {
	Q14PartitionKey month = 0;
	Q14AccT promo_revenue_delta = 0;
};

struct Q14PendingRowLightOp {
	RowSnapshot snapshot;
	int8_t sign = 0;
};

enum class Q14PendingPartLightKind : uint8_t { InsertDelete, PromoFlip };

struct Q14PendingPartLightOp {
	Q14PendingPartLightKind kind = Q14PendingPartLightKind::InsertDelete;
	int64_t partkey = 0;
	bool old_promo = false;
	bool new_promo = false;
	int8_t sign = 0;
};

constexpr uint64_t kQ14MaxLightCaptureRows = 1024;

std::optional<RowSnapshot> ReadRowSnapshot(const duckdb::TableMutationRow &row,
                                           const aux_state::TableDependency &dependency, bool use_new_value) {
	const auto *shipdate_column = FindColumn(dependency, "l_shipdate");
	const auto *partkey_column = FindColumn(dependency, "l_partkey");
	const auto *price_column = FindColumn(dependency, "l_extendedprice");
	const auto *discount_column = FindColumn(dependency, "l_discount");
	if (!shipdate_column || !partkey_column || !price_column || !discount_column) {
		return std::nullopt;
	}
	auto shipdate = PickDateColumn(row, *shipdate_column, use_new_value);
	auto partkey = PickInt64Column(row, *partkey_column, use_new_value);
	auto price = PickInt64Column(row, *price_column, use_new_value);
	auto discount = PickInt64Column(row, *discount_column, use_new_value);
	if (!shipdate || !partkey || !price || !discount) {
		return std::nullopt;
	}
	RowSnapshot snapshot;
	snapshot.month = PartitionKeyFromDate(*shipdate);
	snapshot.partkey = *partkey;
	snapshot.price = *price;
	snapshot.discount = *discount;
	return snapshot;
}

bool UpdateTouchesRelevantColumns(const duckdb::TableMutationEvent &event,
                                  const aux_state::TableDependency &dependency) {
	for (const auto &updated : event.updated_columns) {
		if (dependency.WatchesPhysicalIndex(updated.index)) {
			return true;
		}
	}
	return false;
}

void ApplyDeltaToTotals(Q14MonthTotals &totals, const Q14PartitionDelta &delta) {
	totals.total_revenue_raw += delta.total_revenue_delta;
	totals.promo_revenue_raw += delta.promo_revenue_delta;
	totals.row_count += delta.row_count_delta;
	if (totals.row_count < 0) {
		totals.row_count = 0;
	}
	totals.saw_contribution = totals.row_count > 0;
}

std::unordered_map<Q14PartitionKey, Q14PartitionDelta> FoldRowOps(const std::vector<Q14PendingRowOp> &ops) {
	std::unordered_map<Q14PartitionKey, Q14PartitionDelta> folded;
	for (const auto &op : ops) {
		const auto revenue = DiscountedRevenueRaw(op.price, op.discount) * static_cast<Q14AccT>(op.sign);
		auto &delta = folded[op.month];
		delta.total_revenue_delta += revenue;
		delta.row_count_delta += op.sign;
		if (op.is_promo) {
			delta.promo_revenue_delta += revenue;
		}
	}
	return folded;
}

std::unordered_map<Q14PartitionKey, Q14PartitionDelta> FoldIncrementalOps(
    const std::vector<Q14PendingRowOp> &row_ops, const std::vector<Q14PendingPromoRevenueDelta> &promo_ops) {
	auto folded = FoldRowOps(row_ops);
	for (const auto &promo_op : promo_ops) {
		folded[promo_op.month].promo_revenue_delta += promo_op.promo_revenue_delta;
	}
	return folded;
}

NativeScanSpec CreateLineitemScanForPartkey(int64_t partkey) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_discount", "discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	};
	scan_spec.filters = {
	    {"l_partkey", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value::BIGINT(partkey)},
	};
	return scan_spec;
}

void NotePartition(aux_state::InvalidationPlan &plan, Q14PartitionKey month) {
	plan.scope = aux_state::InvalidationScope::PartitionSet;
	plan.partitions.insert(month);
}

bool TryNotePartitionFromRow(const duckdb::TableMutationRow &row, const aux_state::ColumnRef &shipdate_column,
                             aux_state::InvalidationPlan &plan) {
	auto shipdate = PickDateColumn(row, shipdate_column, false);
	if (!shipdate) {
		shipdate = PickDateColumn(row, shipdate_column, true);
	}
	if (!shipdate) {
		return false;
	}
	NotePartition(plan, PartitionKeyFromDate(*shipdate));
	return true;
}

void ScanLineitemsForPartkey(duckdb::ClientContext &context, int64_t partkey,
                             const std::function<void(Q14PartitionKey, int64_t, int64_t)> &row_fn) {
	auto bind_data = BindNativeScan(context, CreateLineitemScanForPartkey(partkey));
	auto source = CreateNativeChunkSource(context, *bind_data);
	while (source->Next()) {
		auto &chunk = source->Chunk();
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
			const auto price = duckdb::UnifiedVectorFormat::GetData<int64_t>(prices)[price_idx];
			const auto discount = duckdb::UnifiedVectorFormat::GetData<int64_t>(discounts)[discount_idx];
			const auto shipdate = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdates)[shipdate_idx];
			row_fn(PartitionKeyFromDate(shipdate), price, discount);
		}
	}
}

} // namespace

struct Q14IncrementalTxnBuffer::Impl {
	std::vector<Q14PendingRowOp> row_ops;
	std::vector<Q14PendingPromoRevenueDelta> promo_ops;
	std::vector<Q14PendingRowLightOp> pending_row_light_ops;
	std::vector<Q14PendingPartLightOp> pending_part_light_ops;
	aux_state::InvalidationPlan extra_partitions;
	uint64_t cheap_rows_touched = 0;
	uint64_t estimated_lineitem_scan_rows = 0;
	uint64_t cheap_part_fanout = 0;
	bool poisoned = false;
	bool heavy_capture_done = false;
	bool bulk_mutation = false;
};

void Q14IncrementalTxnBuffer::Clear() {
	if (!impl) {
		impl = std::make_unique<Impl>();
	}
	impl->row_ops.clear();
	impl->promo_ops.clear();
	impl->pending_row_light_ops.clear();
	impl->pending_part_light_ops.clear();
	impl->extra_partitions = aux_state::InvalidationPlan {};
	impl->cheap_rows_touched = 0;
	impl->estimated_lineitem_scan_rows = 0;
	impl->cheap_part_fanout = 0;
	impl->poisoned = false;
	impl->heavy_capture_done = false;
	impl->bulk_mutation = false;
}

bool Q14IncrementalTxnBuffer::HasPendingChanges() const {
	return impl && (!impl->row_ops.empty() || !impl->promo_ops.empty() || impl->cheap_rows_touched > 0 ||
	                !impl->pending_row_light_ops.empty() || !impl->pending_part_light_ops.empty());
}

uint64_t Q14IncrementalTxnBuffer::CheapRowsTouched() const {
	return impl ? impl->cheap_rows_touched : 0;
}

uint64_t Q14IncrementalTxnBuffer::CheapPartFanout() const {
	return impl ? impl->cheap_part_fanout : 0;
}

uint64_t Q14IncrementalTxnBuffer::EstimatedLineitemScanRows() const {
	return impl ? impl->estimated_lineitem_scan_rows : 0;
}

bool Q14IncrementalTxnBuffer::HeavyCaptureDone() const {
	return impl && impl->heavy_capture_done;
}

bool Q14IncrementalTxnBuffer::BulkMutation() const {
	return impl && impl->bulk_mutation;
}

aux_state::InvalidationPlan Q14IncrementalTxnBuffer::ExtraInvalidationPlan() const {
	if (!impl) {
		return {};
	}
	return impl->extra_partitions;
}

Q14IncrementalTxnBuffer::Q14IncrementalTxnBuffer() : impl(std::make_unique<Impl>()) {
}

Q14IncrementalTxnBuffer::~Q14IncrementalTxnBuffer() = default;

Q14IncrementalTxnBuffer::Q14IncrementalTxnBuffer(Q14IncrementalTxnBuffer &&) noexcept = default;
Q14IncrementalTxnBuffer &Q14IncrementalTxnBuffer::operator=(Q14IncrementalTxnBuffer &&) noexcept = default;

bool Q14FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                   const std::vector<aux_state::TableDependency> &dependencies,
                                   Q14IncrementalTxnBuffer &buffer);

void MaybeMergePendingLightOps(duckdb::ClientContext &context,
                               const std::vector<aux_state::TableDependency> &dependencies,
                               Q14IncrementalTxnBuffer &buffer) {
	if (!buffer.impl || buffer.impl->poisoned || buffer.impl->bulk_mutation) {
		return;
	}
	if (buffer.impl->pending_row_light_ops.empty() && buffer.impl->pending_part_light_ops.empty()) {
		return;
	}
	if (buffer.impl->cheap_rows_touched > kQ14MaxLightCaptureRows) {
		return;
	}
	if (!buffer.impl->pending_part_light_ops.empty() &&
	    buffer.impl->cheap_part_fanout > GetQ14CostModelParams().apply_delta_max_part_fanout) {
		return;
	}
	Q14FinalizeIncrementalCapture(context, dependencies, buffer);
}

void MaybeEagerFinalizeCapture(duckdb::ClientContext &context,
                               const std::vector<aux_state::TableDependency> &dependencies,
                               Q14IncrementalTxnBuffer &buffer) {
	MaybeMergePendingLightOps(context, dependencies, buffer);
}

void Q14AccumulateLineitemMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                        const std::vector<aux_state::TableDependency> &dependencies,
                                        Q14IncrementalTxnBuffer &buffer) {
	(void)context;
	if (!buffer.impl) {
		buffer.impl = std::make_unique<Q14IncrementalTxnBuffer::Impl>();
	}
	if (buffer.impl->poisoned) {
		return;
	}
	const auto *lineitem = FindTableDependency(dependencies, "lineitem");
	if (!lineitem || event.table_oid != lineitem->table_oid) {
		return;
	}

	const auto *shipdate_column = FindColumn(*lineitem, "l_shipdate");

	auto touch_rows = [&](size_t row_count) {
		buffer.impl->cheap_rows_touched += row_count;
		if (buffer.impl->cheap_rows_touched > kQ14MaxLightCaptureRows) {
			buffer.impl->bulk_mutation = true;
		}
	};

	auto note_snapshot = [&](const RowSnapshot &snapshot) { NotePartition(buffer.impl->extra_partitions, snapshot.month); };

	auto note_row_partition = [&](const duckdb::TableMutationRow &row) {
		if (shipdate_column) {
			TryNotePartitionFromRow(row, *shipdate_column, buffer.impl->extra_partitions);
		}
	};

	auto append_light_row = [&](const RowSnapshot &snapshot, int8_t sign) {
		note_snapshot(snapshot);
		if (buffer.impl->bulk_mutation) {
			return;
		}
		buffer.impl->pending_row_light_ops.push_back(Q14PendingRowLightOp {snapshot, sign});
	};

	switch (event.kind) {
	case duckdb::TableMutationKind::INSERT:
		touch_rows(event.rows.size());
		for (const auto &row : event.rows) {
			note_row_partition(row);
			if (buffer.impl->bulk_mutation) {
				continue;
			}
			auto snapshot = ReadRowSnapshot(row, *lineitem, true);
			if (!snapshot) {
				buffer.impl->poisoned = true;
				buffer.impl->pending_row_light_ops.clear();
				return;
			}
			append_light_row(*snapshot, 1);
		}
		break;
	case duckdb::TableMutationKind::DELETE:
		touch_rows(event.rows.size());
		for (const auto &row : event.rows) {
			note_row_partition(row);
			if (buffer.impl->bulk_mutation) {
				continue;
			}
			auto snapshot = ReadRowSnapshot(row, *lineitem, false);
			if (!snapshot) {
				buffer.impl->poisoned = true;
				buffer.impl->pending_row_light_ops.clear();
				return;
			}
			append_light_row(*snapshot, -1);
		}
		break;
	case duckdb::TableMutationKind::UPDATE:
		if (!UpdateTouchesRelevantColumns(event, *lineitem)) {
			return;
		}
		touch_rows(event.rows.size());
		for (const auto &row : event.rows) {
			note_row_partition(row);
			if (buffer.impl->bulk_mutation) {
				continue;
			}
			auto old_snapshot = ReadRowSnapshot(row, *lineitem, false);
			auto new_snapshot = ReadRowSnapshot(row, *lineitem, true);
			if (!old_snapshot || !new_snapshot) {
				buffer.impl->poisoned = true;
				buffer.impl->pending_row_light_ops.clear();
				return;
			}
			append_light_row(*old_snapshot, -1);
			append_light_row(*new_snapshot, 1);
		}
		break;
	default:
		return;
	}
	MaybeEagerFinalizeCapture(context, dependencies, buffer);
}

void Q14AccumulatePartMutationLight(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                    const std::vector<aux_state::TableDependency> &dependencies,
                                    Q14IncrementalTxnBuffer &buffer) {
	(void)context;
	if (!buffer.impl) {
		buffer.impl = std::make_unique<Q14IncrementalTxnBuffer::Impl>();
	}
	if (buffer.impl->poisoned) {
		return;
	}
	const auto *part = FindTableDependency(dependencies, "part");
	if (!part || event.table_oid != part->table_oid) {
		return;
	}
	const auto *partkey_column = FindColumn(*part, "p_partkey");
	const auto *type_column = FindColumn(*part, "p_type");
	if (!partkey_column || !type_column) {
		buffer.impl->poisoned = true;
		return;
	}

	auto touch_rows = [&](size_t row_count) {
		buffer.impl->cheap_rows_touched += row_count;
	};

	auto note_part = [&](int64_t) { buffer.impl->cheap_part_fanout++; };

	switch (event.kind) {
	case duckdb::TableMutationKind::INSERT:
		touch_rows(event.rows.size());
		for (const auto &row : event.rows) {
			auto partkey = PickInt64Column(row, *partkey_column, true);
			auto type_value = PickValueColumn(row, *type_column, true);
			if (!partkey || !type_value) {
				buffer.impl->poisoned = true;
				return;
			}
			note_part(*partkey);
			buffer.impl->pending_part_light_ops.push_back(
			    Q14PendingPartLightOp {Q14PendingPartLightKind::InsertDelete, *partkey, false, IsPromoValue(*type_value), 1});
		}
		break;
	case duckdb::TableMutationKind::DELETE:
		touch_rows(event.rows.size());
		for (const auto &row : event.rows) {
			auto partkey = PickInt64Column(row, *partkey_column, false);
			auto type_value = PickValueColumn(row, *type_column, false);
			if (!partkey || !type_value) {
				buffer.impl->poisoned = true;
				return;
			}
			note_part(*partkey);
			buffer.impl->pending_part_light_ops.push_back(
			    Q14PendingPartLightOp {Q14PendingPartLightKind::InsertDelete, *partkey, IsPromoValue(*type_value), false, -1});
		}
		break;
	case duckdb::TableMutationKind::UPDATE:
		if (!UpdateTouchesRelevantColumns(event, *part)) {
			return;
		}
		touch_rows(event.rows.size());
		for (const auto &row : event.rows) {
			auto partkey = PickInt64Column(row, *partkey_column, false);
			auto old_type = PickValueColumn(row, *type_column, false);
			auto new_type = PickValueColumn(row, *type_column, true);
			if (!partkey || !old_type || !new_type) {
				buffer.impl->poisoned = true;
				return;
			}
			note_part(*partkey);
			buffer.impl->pending_part_light_ops.push_back(Q14PendingPartLightOp {Q14PendingPartLightKind::PromoFlip, *partkey,
			                                                                     IsPromoValue(*old_type), IsPromoValue(*new_type), 0});
		}
		break;
	default:
		return;
	}
	buffer.impl->estimated_lineitem_scan_rows = std::max(buffer.impl->estimated_lineitem_scan_rows, buffer.impl->cheap_part_fanout);
	MaybeEagerFinalizeCapture(context, dependencies, buffer);
}

bool Q14FinalizeIncrementalCapture(duckdb::ClientContext &context,
                                   const std::vector<aux_state::TableDependency> &dependencies,
                                   Q14IncrementalTxnBuffer &buffer) {
	if (!buffer.impl || buffer.impl->poisoned) {
		return false;
	}
	if (buffer.impl->bulk_mutation) {
		return false;
	}
	if (buffer.impl->pending_row_light_ops.empty() && buffer.impl->pending_part_light_ops.empty()) {
		return !buffer.impl->row_ops.empty() || !buffer.impl->promo_ops.empty();
	}

	const auto *lineitem = FindTableDependency(dependencies, "lineitem");
	const auto *part = FindTableDependency(dependencies, "part");

	auto resolve_promo_for_snapshot = [&](const RowSnapshot &row) -> std::optional<bool> {
		NativeScanSpec scan_spec;
		scan_spec.table_name = "part";
		scan_spec.projected_columns = {
		    {"p_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
		    {"p_type", "type", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
		};
		scan_spec.filters = {
		    {"p_partkey", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value::BIGINT(row.partkey)},
		};
		auto bind_data = BindNativeScan(context, scan_spec);
		auto source = CreateNativeChunkSource(context, *bind_data);
		while (source->Next()) {
			auto &chunk = source->Chunk();
			duckdb::UnifiedVectorFormat types;
			chunk.data[1].ToUnifiedFormat(chunk.size(), types);
			for (duckdb::idx_t scan_row = 0; scan_row < chunk.size(); scan_row++) {
				const auto type_idx = types.sel->get_index(scan_row);
				if (!types.validity.RowIsValid(type_idx)) {
					return false;
				}
				const auto &type = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(types)[type_idx];
				return IsPromoType(type);
			}
		}
		return std::nullopt;
	};

	for (const auto &pending : buffer.impl->pending_row_light_ops) {
		auto promo = resolve_promo_for_snapshot(pending.snapshot);
		if (!promo) {
			buffer.impl->poisoned = true;
			buffer.impl->row_ops.clear();
			return false;
		}
		buffer.impl->row_ops.push_back(
		    Q14PendingRowOp {pending.sign, pending.snapshot.month, pending.snapshot.partkey, pending.snapshot.price,
		                     pending.snapshot.discount, *promo});
	}

	auto append_row_ops_for_partkey = [&](int64_t partkey, bool is_promo, int8_t sign) {
		ScanLineitemsForPartkey(context, partkey, [&](Q14PartitionKey month, int64_t price, int64_t discount) {
			buffer.impl->row_ops.push_back(Q14PendingRowOp {sign, month, partkey, price, discount, is_promo});
			NotePartition(buffer.impl->extra_partitions, month);
			buffer.impl->estimated_lineitem_scan_rows++;
		});
	};

	auto append_promo_flip = [&](int64_t partkey, bool old_promo, bool new_promo) {
		if (old_promo == new_promo) {
			return;
		}
		ScanLineitemsForPartkey(context, partkey, [&](Q14PartitionKey month, int64_t price, int64_t discount) {
			const auto revenue = DiscountedRevenueRaw(price, discount);
			Q14AccT delta = 0;
			if (new_promo && !old_promo) {
				delta = revenue;
			} else if (old_promo && !new_promo) {
				delta = -revenue;
			}
			buffer.impl->promo_ops.push_back(Q14PendingPromoRevenueDelta {month, delta});
			NotePartition(buffer.impl->extra_partitions, month);
			buffer.impl->estimated_lineitem_scan_rows++;
		});
	};

	for (const auto &pending : buffer.impl->pending_part_light_ops) {
		if (pending.kind == Q14PendingPartLightKind::InsertDelete) {
			append_row_ops_for_partkey(pending.partkey, pending.new_promo, pending.sign);
		} else {
			append_promo_flip(pending.partkey, pending.old_promo, pending.new_promo);
		}
	}

	buffer.impl->pending_row_light_ops.clear();
	buffer.impl->pending_part_light_ops.clear();
	buffer.impl->heavy_capture_done = true;
	if (buffer.impl->row_ops.empty() && buffer.impl->promo_ops.empty()) {
		buffer.impl->cheap_rows_touched = 0;
		buffer.impl->cheap_part_fanout = 0;
		buffer.impl->estimated_lineitem_scan_rows = 0;
	}
	(void)lineitem;
	(void)part;
	return !buffer.impl->poisoned;
}

void Q14AccumulateLineitemMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                                   const std::vector<aux_state::TableDependency> &dependencies,
                                   Q14IncrementalTxnBuffer &buffer) {
	Q14AccumulateLineitemMutationLight(context, event, dependencies, buffer);
}

void Q14AccumulatePartMutation(duckdb::ClientContext &context, const duckdb::TableMutationEvent &event,
                               const std::vector<aux_state::TableDependency> &dependencies,
                               Q14IncrementalTxnBuffer &buffer) {
	Q14AccumulatePartMutationLight(context, event, dependencies, buffer);
}

bool Q14IncrementalTxnBuffer::IsPoisoned() const {
	return impl && impl->poisoned;
}

uint64_t Q14IncrementalTxnBuffer::RowOpCount() const {
	return impl ? impl->row_ops.size() : 0;
}

uint64_t Q14IncrementalTxnBuffer::PromoOpCount() const {
	return impl ? impl->promo_ops.size() : 0;
}

uint64_t Q14IncrementalTxnBuffer::PendingLightOpCount() const {
	if (!impl) {
		return 0;
	}
	return impl->pending_row_light_ops.size() + impl->pending_part_light_ops.size();
}

bool ComputeQ14IncrementalDelta(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                              const aux_state::InvalidationPlan &plan, Q14IncrementalTxnBuffer &buffer,
                              Q14ComputedIncrementalDelta &out) {
	(void)context;
	const auto start = std::chrono::steady_clock::now();
	out = Q14ComputedIncrementalDelta {};
	if (!buffer.impl || buffer.impl->poisoned) {
		return false;
	}
	if (plan.scope == aux_state::InvalidationScope::AllPartitions) {
		return false;
	}
	if (plan.scope == aux_state::InvalidationScope::None && !buffer.HasPendingChanges()) {
		out.success = true;
		return true;
	}

	auto folded = FoldIncrementalOps(buffer.impl->row_ops, buffer.impl->promo_ops);
	std::unordered_set<Q14PartitionKey> partitions;
	if (plan.scope == aux_state::InvalidationScope::PartitionSet) {
		for (auto partition : plan.partitions) {
			partitions.insert(partition);
		}
	}
	for (const auto &entry : folded) {
		partitions.insert(entry.first);
	}
	if (partitions.empty()) {
		out.success = true;
		return true;
	}

	for (auto partition : partitions) {
		const auto folded_it = folded.find(partition);
		if (folded_it == folded.end()) {
			continue;
		}
		const auto status = store.GetStatus(partition);
		if (status == aux_state::PartitionStatus::Invalid) {
			return false;
		}
		Q14MonthTotals totals;
		if (status == aux_state::PartitionStatus::Valid) {
			auto payload = store.GetReadablePayload(partition);
			if (!payload) {
				return false;
			}
			totals = *std::static_pointer_cast<const Q14MonthTotals>(payload);
		}
		ApplyDeltaToTotals(totals, folded_it->second);
		aux_state::maintenance::StagedPartitionUpdate update;
		update.key = partition;
		update.payload = std::make_shared<Q14MonthTotals>(totals);
		update.expected_partition_version = store.GetPartitionVersion(partition);
		out.partition_updates.push_back(std::move(update));
		out.affected_partitions.scope = aux_state::InvalidationScope::PartitionSet;
		out.affected_partitions.partitions.insert(partition);
	}
	out.success = true;
	const auto end = std::chrono::steady_clock::now();
	const auto ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
	RecordQ14IncrementalApplyAttempt(true, ns);
	return true;
}

void PublishQ14IncrementalDelta(aux_state::PartitionedStateStore &store, const Q14ComputedIncrementalDelta &delta,
                                uint64_t valid_through_mutation_epoch) {
	if (!delta.success) {
		return;
	}
	aux_state::maintenance::PublishValidatedPartitionUpdates(store, delta.partition_updates, valid_through_mutation_epoch);
}

bool TryApplyQ14IncrementalPreCommit(duckdb::ClientContext &context, aux_state::PartitionedStateStore &store,
                                    const aux_state::InvalidationPlan &plan, Q14IncrementalTxnBuffer &buffer) {
	Q14ComputedIncrementalDelta delta;
	if (!ComputeQ14IncrementalDelta(context, store, plan, buffer, delta)) {
		RecordQ14IncrementalApplyAttempt(false, 0);
		return false;
	}
	PublishQ14IncrementalDelta(store, delta, 0);
	return true;
}

Q14IncrementalTxnBuffer &GetQ14IncrementalTxnBuffer(aux_state::TxnInvalidationBuffer &txn_buffer) {
	auto &base = txn_buffer.GetOrCreateIncrementalBuffer(kQ14AuxAcceleratorId);
	auto *q14 = dynamic_cast<Q14IncrementalTxnBuffer *>(&base);
	if (!q14) {
		throw std::runtime_error("incremental buffer for tpch_q14_aux has unexpected type");
	}
	return *q14;
}

aux_state::InvalidationPlan AccumulateQ14Mutation(duckdb::ClientContext &context,
                                                  const duckdb::TableMutationEvent &event,
                                                  const std::vector<aux_state::TableDependency> &dependencies,
                                                  aux_state::TxnInvalidationBuffer &txn_buffer) {
	auto &incremental = GetQ14IncrementalTxnBuffer(txn_buffer);
	const bool dependent = IsQ14DependentTableMutation(event, dependencies);

	const auto *lineitem = FindTableDependency(dependencies, "lineitem");
	const auto *part = FindTableDependency(dependencies, "part");
	if (lineitem && event.table_oid == lineitem->table_oid) {
		Q14AccumulateLineitemMutationLight(context, event, dependencies, incremental);
	} else if (part && event.table_oid == part->table_oid) {
		Q14AccumulatePartMutationLight(context, event, dependencies, incremental);
	}

	if (dependent) {
		RecordQ14DependentTableMutation(incremental.ExtraInvalidationPlan().IsEmpty());
		RecordQ14WorkloadRelevantWrite();
	}

	return incremental.ExtraInvalidationPlan();
}

} // namespace accelerator_codegen
