#include "accelerator_codegen/tpch_q1_aux.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/local_storage.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kDecimalScale2 = 2;
constexpr uint8_t kDecimalScale4 = 4;
constexpr uint8_t kDecimalScale6 = 6;
constexpr uint8_t kDecimalWidth38 = 38;
constexpr duckdb::idx_t kQ1OutputBatchSize = 4;

#if defined(__SIZEOF_INT128__)
using wide_hugeint_t = __int128_t;

duckdb::hugeint_t FromWideHugeint(wide_hugeint_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}

inline void AddValue(wide_hugeint_t &accumulator, int64_t value) {
	accumulator += static_cast<wide_hugeint_t>(value);
}

inline void AddWide(wide_hugeint_t &accumulator, wide_hugeint_t value) {
	accumulator += value;
}
#else
using wide_hugeint_t = duckdb::hugeint_t;

duckdb::hugeint_t FromWideHugeint(wide_hugeint_t value) {
	return value;
}

inline void AddValue(wide_hugeint_t &accumulator, int64_t input) {
	auto addend = duckdb::Hugeint::Convert(input);
	if (!duckdb::Hugeint::TryAddInPlace(accumulator, addend)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q1 aux aggregate");
	}
}

inline void AddWide(wide_hugeint_t &accumulator, wide_hugeint_t value) {
	if (!duckdb::Hugeint::TryAddInPlace(accumulator, value)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q1 aux aggregate");
	}
}
#endif

double HugeintScaleToDouble(wide_hugeint_t value, uint8_t scale, uint64_t count) {
	auto as_hugeint = FromWideHugeint(value);
	auto numeric = duckdb::Hugeint::Cast<double>(as_hugeint);
	double scaled = numeric;
	while (scale > 0) {
		scaled /= 10.0;
		scale--;
	}
	return scaled / static_cast<double>(count);
}

uint16_t PackGroupKey(duckdb::string_t returnflag, duckdb::string_t linestatus) {
	if (returnflag.GetSize() != 1 || linestatus.GetSize() != 1) {
		throw duckdb::InvalidInputException("Q1 expects single-character l_returnflag and l_linestatus values");
	}
	auto lhs = static_cast<uint8_t>(returnflag.GetDataUnsafe()[0]);
	auto rhs = static_cast<uint8_t>(linestatus.GetDataUnsafe()[0]);
	return static_cast<uint16_t>((static_cast<uint16_t>(lhs) << 8) | rhs);
}

std::string UnpackLeft(uint16_t key) {
	return std::string(1, static_cast<char>(key >> 8));
}

std::string UnpackRight(uint16_t key) {
	return std::string(1, static_cast<char>(key & 0xFF));
}

bool ReadBoolEnv(const char *name, bool fallback) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		return fallback;
	}
	return value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y';
}

struct Accumulators {
	wide_hugeint_t sum_qty = 0;
	wide_hugeint_t sum_base_price = 0;
	wide_hugeint_t sum_disc_price = 0;
	wide_hugeint_t sum_charge = 0;
	wide_hugeint_t sum_discount = 0;
	uint64_t count = 0;

	void AddRow(int64_t quantity_raw, int64_t extendedprice_raw, int64_t discount_raw, int64_t tax_raw) {
		AddValue(sum_qty, quantity_raw);
		AddValue(sum_base_price, extendedprice_raw);
		AddValue(sum_discount, discount_raw);
		const auto one_minus_discount_raw = static_cast<int64_t>(100 - discount_raw);
		const auto disc_price_raw = extendedprice_raw * one_minus_discount_raw;
		AddValue(sum_disc_price, disc_price_raw);
		const auto one_plus_tax_raw = static_cast<int64_t>(100 + tax_raw);
		AddValue(sum_charge, disc_price_raw * one_plus_tax_raw);
		count += 1;
	}

	void Add(const Accumulators &other) {
		AddWide(sum_qty, other.sum_qty);
		AddWide(sum_base_price, other.sum_base_price);
		AddWide(sum_disc_price, other.sum_disc_price);
		AddWide(sum_charge, other.sum_charge);
		AddWide(sum_discount, other.sum_discount);
		count += other.count;
	}
};

struct OutputRow {
	uint16_t key = 0;
	duckdb::hugeint_t sum_qty {};
	duckdb::hugeint_t sum_base_price {};
	duckdb::hugeint_t sum_disc_price {};
	duckdb::hugeint_t sum_charge {};
	double avg_qty = 0;
	double avg_price = 0;
	double avg_disc = 0;
	int64_t count_order = 0;
};

struct PrefixSnapshot {
	duckdb::DataTable *table = nullptr;
	duckdb::idx_t total_rows = 0;
	int64_t fingerprint_shipdate_sum = 0;
	int64_t fingerprint_qty_sum = 0;
	std::vector<duckdb::date_t> dates;
	std::vector<uint16_t> group_keys;
	std::vector<Accumulators> prefix;
	duckdb::idx_t memory_bytes = 0;
	duckdb::idx_t build_ns = 0;
	duckdb::idx_t source_rows = 0;
};

std::mutex g_aux_mu;
std::unique_ptr<PrefixSnapshot> g_aux_snapshot;
Q1AuxStateDiagnostics g_aux_diagnostics;

duckdb::TableCatalogEntry &LookupLineitem(duckdb::ClientContext &context) {
	return duckdb::Catalog::GetEntry<duckdb::TableCatalogEntry>(context, INVALID_CATALOG, DEFAULT_SCHEMA, "lineitem");
}

void RejectIfLocalWrites(duckdb::ClientContext &context, duckdb::DataTable &table) {
	auto &transaction = duckdb::DuckTransaction::Get(context, table.db);
	if (duckdb::LocalStorage::Get(transaction).Find(table)) {
		g_aux_diagnostics.refused_local_writes = true;
		throw duckdb::InvalidInputException(
		    "Q1 aux prefix state rejects queries with uncommitted local modifications on lineitem "
		    "(immutable snapshot PoC; rebuild after commit or disable ACCEL_Q1_AUX_STATE)");
	}
}

duckdb::idx_t EstimateMemoryBytes(const PrefixSnapshot &snap) {
	duckdb::idx_t bytes = sizeof(PrefixSnapshot);
	bytes += snap.dates.size() * sizeof(duckdb::date_t);
	bytes += snap.group_keys.size() * sizeof(uint16_t);
	bytes += snap.prefix.size() * sizeof(Accumulators);
	return bytes;
}

void ConsumeBuildChunk(duckdb::DataChunk &chunk, std::map<std::pair<int32_t, uint16_t>, Accumulators> &deltas,
                       int64_t &shipdate_sum, int64_t &qty_sum, duckdb::idx_t &rows) {
	const auto count = chunk.size();
	if (count == 0) {
		return;
	}
	duckdb::UnifiedVectorFormat shipdate_data;
	duckdb::UnifiedVectorFormat quantity_data;
	duckdb::UnifiedVectorFormat extendedprice_data;
	duckdb::UnifiedVectorFormat discount_data;
	duckdb::UnifiedVectorFormat tax_data;
	duckdb::UnifiedVectorFormat returnflag_data;
	duckdb::UnifiedVectorFormat linestatus_data;
	chunk.data[Q1LineitemChunkLayout::SHIPDATE_INDEX].ToUnifiedFormat(count, shipdate_data);
	chunk.data[Q1LineitemChunkLayout::QUANTITY_INDEX].ToUnifiedFormat(count, quantity_data);
	chunk.data[Q1LineitemChunkLayout::EXTENDEDPRICE_INDEX].ToUnifiedFormat(count, extendedprice_data);
	chunk.data[Q1LineitemChunkLayout::DISCOUNT_INDEX].ToUnifiedFormat(count, discount_data);
	chunk.data[Q1LineitemChunkLayout::TAX_INDEX].ToUnifiedFormat(count, tax_data);
	chunk.data[Q1LineitemChunkLayout::RETURNFLAG_INDEX].ToUnifiedFormat(count, returnflag_data);
	chunk.data[Q1LineitemChunkLayout::LINESTATUS_INDEX].ToUnifiedFormat(count, linestatus_data);

	auto *shipdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdate_data);
	auto *quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_data);
	auto *extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_data);
	auto *discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_data);
	auto *taxes = duckdb::UnifiedVectorFormat::GetData<int64_t>(tax_data);
	auto *returnflags = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(returnflag_data);
	auto *linestatuses = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(linestatus_data);

	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto s_idx = shipdate_data.sel->get_index(row);
		const auto q_idx = quantity_data.sel->get_index(row);
		const auto e_idx = extendedprice_data.sel->get_index(row);
		const auto d_idx = discount_data.sel->get_index(row);
		const auto t_idx = tax_data.sel->get_index(row);
		const auto rf_idx = returnflag_data.sel->get_index(row);
		const auto ls_idx = linestatus_data.sel->get_index(row);
		if (!shipdate_data.validity.RowIsValid(s_idx) || !quantity_data.validity.RowIsValid(q_idx) ||
		    !extendedprice_data.validity.RowIsValid(e_idx) || !discount_data.validity.RowIsValid(d_idx) ||
		    !tax_data.validity.RowIsValid(t_idx) || !returnflag_data.validity.RowIsValid(rf_idx) ||
		    !linestatus_data.validity.RowIsValid(ls_idx)) {
			continue;
		}
		const auto shipdate = shipdates[s_idx];
		const auto quantity = quantities[q_idx];
		const auto group_key = PackGroupKey(returnflags[rf_idx], linestatuses[ls_idx]);
		deltas[{shipdate.days, group_key}].AddRow(quantity, extendedprices[e_idx], discounts[d_idx], taxes[t_idx]);
		shipdate_sum += static_cast<int64_t>(shipdate.days);
		qty_sum += quantity;
		rows += 1;
	}
}

std::unique_ptr<PrefixSnapshot> BuildSnapshot(duckdb::ClientContext &context) {
	auto &table_entry = LookupLineitem(context);
	auto &table = table_entry.GetStorage();
	RejectIfLocalWrites(context, table);

	const auto build_start = std::chrono::steady_clock::now();
	auto scan_bind = BindNativeScan(context, CreateQ1AuxBuildScanSpec());
	auto source = CreateNativeChunkSource(context, *scan_bind);

	std::map<std::pair<int32_t, uint16_t>, Accumulators> deltas;
	int64_t shipdate_sum = 0;
	int64_t qty_sum = 0;
	duckdb::idx_t source_rows = 0;
	while (source->Next()) {
		ConsumeBuildChunk(source->Chunk(), deltas, shipdate_sum, qty_sum, source_rows);
	}

	std::vector<duckdb::date_t> dates;
	std::vector<uint16_t> group_keys;
	dates.reserve(deltas.size());
	for (const auto &entry : deltas) {
		const duckdb::date_t date(entry.first.first);
		if (dates.empty() || dates.back() != date) {
			dates.push_back(date);
		}
		group_keys.push_back(entry.first.second);
	}
	std::sort(group_keys.begin(), group_keys.end());
	group_keys.erase(std::unique(group_keys.begin(), group_keys.end()), group_keys.end());

	const auto n_dates = dates.size();
	const auto n_groups = group_keys.size();
	auto snap = std::make_unique<PrefixSnapshot>();
	snap->table = &table;
	snap->total_rows = table.GetTotalRows();
	snap->fingerprint_shipdate_sum = shipdate_sum;
	snap->fingerprint_qty_sum = qty_sum;
	snap->dates = std::move(dates);
	snap->group_keys = std::move(group_keys);
	snap->prefix.assign(n_dates * n_groups, Accumulators {});
	snap->source_rows = source_rows;

	std::vector<Accumulators> running(n_groups);
	duckdb::idx_t date_idx = 0;
	for (const auto date : snap->dates) {
		for (duckdb::idx_t g = 0; g < n_groups; g++) {
			const auto key = snap->group_keys[g];
			auto it = deltas.find({date.days, key});
			if (it != deltas.end()) {
				running[g].Add(it->second);
			}
		}
		for (duckdb::idx_t g = 0; g < n_groups; g++) {
			snap->prefix[date_idx * n_groups + g] = running[g];
		}
		date_idx += 1;
	}

	snap->memory_bytes = EstimateMemoryBytes(*snap);
	snap->build_ns = static_cast<duckdb::idx_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - build_start).count());
	return snap;
}

std::vector<OutputRow> LookupCutoff(const PrefixSnapshot &snap, duckdb::date_t cutoff) {
	std::vector<OutputRow> rows;
	if (snap.dates.empty()) {
		return rows;
	}
	auto it = std::upper_bound(snap.dates.begin(), snap.dates.end(), cutoff,
	                           [](duckdb::date_t value, duckdb::date_t elem) { return value.days < elem.days; });
	if (it == snap.dates.begin()) {
		return rows;
	}
	const auto date_idx = static_cast<duckdb::idx_t>(std::distance(snap.dates.begin(), it) - 1);
	const auto n_groups = snap.group_keys.size();
	rows.reserve(n_groups);
	for (duckdb::idx_t g = 0; g < n_groups; g++) {
		const auto &acc = snap.prefix[date_idx * n_groups + g];
		if (acc.count == 0) {
			continue;
		}
		OutputRow row;
		row.key = snap.group_keys[g];
		row.sum_qty = FromWideHugeint(acc.sum_qty);
		row.sum_base_price = FromWideHugeint(acc.sum_base_price);
		row.sum_disc_price = FromWideHugeint(acc.sum_disc_price);
		row.sum_charge = FromWideHugeint(acc.sum_charge);
		row.avg_qty = HugeintScaleToDouble(acc.sum_qty, kDecimalScale2, acc.count);
		row.avg_price = HugeintScaleToDouble(acc.sum_base_price, kDecimalScale2, acc.count);
		row.avg_disc = HugeintScaleToDouble(acc.sum_discount, kDecimalScale2, acc.count);
		row.count_order = static_cast<int64_t>(acc.count);
		rows.push_back(row);
	}
	std::sort(rows.begin(), rows.end(), [](const OutputRow &lhs, const OutputRow &rhs) { return lhs.key < rhs.key; });
	return rows;
}

} // namespace

bool Q1AuxStateEnabled() {
	return ReadBoolEnv("ACCEL_Q1_AUX_STATE", false);
}

void InvalidateQ1AuxState() {
	std::lock_guard<std::mutex> guard(g_aux_mu);
	g_aux_snapshot.reset();
	g_aux_diagnostics = {};
}

void BuildQ1AuxState(duckdb::ClientContext &context) {
	std::lock_guard<std::mutex> guard(g_aux_mu);
	g_aux_diagnostics = {};
	g_aux_snapshot = BuildSnapshot(context);
	g_aux_diagnostics.built_this_call = true;
	g_aux_diagnostics.date_entries = g_aux_snapshot->dates.size();
	g_aux_diagnostics.group_entries = g_aux_snapshot->group_keys.size();
	g_aux_diagnostics.prefix_cells = g_aux_snapshot->prefix.size();
	g_aux_diagnostics.source_rows = g_aux_snapshot->source_rows;
	g_aux_diagnostics.memory_bytes = g_aux_snapshot->memory_bytes;
	g_aux_diagnostics.build_ns = g_aux_snapshot->build_ns;
}

const Q1AuxStateDiagnostics &GetQ1AuxStateDiagnostics() {
	return g_aux_diagnostics;
}

NativeScanSpec CreateQ1AuxBuildScanSpec() {
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
	return scan_spec;
}

struct Q1AuxKernel::State {
	std::vector<OutputRow> ordered_rows;
	bool answered = false;
	bool finalized = false;
	duckdb::idx_t output_offset = 0;
};

Q1AuxKernel::Q1AuxKernel(Q1Parameters params_p) : params(std::move(params_p)), state(std::make_unique<State>()) {
}

Q1AuxKernel::~Q1AuxKernel() = default;

void Q1AuxKernel::ConsumeInput(duckdb::idx_t, duckdb::DataChunk &) {
	throw duckdb::InternalException("Q1AuxKernel does not consume DataChunks; call AnswerFromAuxState");
}

void Q1AuxKernel::AnswerFromAuxState(duckdb::ClientContext &context) {
	if (state->answered) {
		return;
	}
	diagnostics.used_aux_path = true;
	const auto lookup_start = std::chrono::steady_clock::now();

	std::lock_guard<std::mutex> guard(g_aux_mu);
	auto &table_entry = LookupLineitem(context);
	auto &table = table_entry.GetStorage();
	RejectIfLocalWrites(context, table);

	if (!g_aux_snapshot || g_aux_snapshot->table != &table) {
		g_aux_snapshot = BuildSnapshot(context);
		diagnostics.built_this_call = true;
		g_aux_diagnostics.built_this_call = true;
		g_aux_diagnostics.date_entries = g_aux_snapshot->dates.size();
		g_aux_diagnostics.group_entries = g_aux_snapshot->group_keys.size();
		g_aux_diagnostics.prefix_cells = g_aux_snapshot->prefix.size();
		g_aux_diagnostics.source_rows = g_aux_snapshot->source_rows;
		g_aux_diagnostics.memory_bytes = g_aux_snapshot->memory_bytes;
		g_aux_diagnostics.build_ns = g_aux_snapshot->build_ns;
	} else if (table.GetTotalRows() != g_aux_snapshot->total_rows) {
		g_aux_diagnostics.refused_stale = true;
		throw duckdb::InvalidInputException(
		    "Q1 aux prefix state is stale (lineitem row count changed from %llu to %llu); rebuild required "
		    "(InvalidateQ1AuxState + BuildQ1AuxState, or restart with a fresh process)",
		    static_cast<unsigned long long>(g_aux_snapshot->total_rows),
		    static_cast<unsigned long long>(table.GetTotalRows()));
	}

	diagnostics.date_entries = g_aux_snapshot->dates.size();
	diagnostics.group_entries = g_aux_snapshot->group_keys.size();
	diagnostics.prefix_cells = g_aux_snapshot->prefix.size();
	diagnostics.source_rows = g_aux_snapshot->source_rows;
	diagnostics.memory_bytes = g_aux_snapshot->memory_bytes;
	diagnostics.build_ns = g_aux_snapshot->build_ns;

	state->ordered_rows = LookupCutoff(*g_aux_snapshot, params.cutoff_date);
	diagnostics.lookup_ns = static_cast<duckdb::idx_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - lookup_start).count());
	g_aux_diagnostics.lookup_ns = diagnostics.lookup_ns;
	g_aux_diagnostics.used_aux_path = true;
	state->answered = true;
}

void Q1AuxKernel::Finalize() {
	if (state->finalized) {
		return;
	}
	if (!state->answered) {
		throw duckdb::InternalException("Q1AuxKernel::Finalize called before AnswerFromAuxState");
	}
	const auto finalize_start = std::chrono::steady_clock::now();
	state->finalized = true;
	diagnostics.finalize_ns = static_cast<duckdb::idx_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - finalize_start).count());
}

bool Q1AuxKernel::ProduceOutput(duckdb::DataChunk &output) {
	if (!state->finalized || state->output_offset >= state->ordered_rows.size()) {
		return false;
	}
	const auto remaining = state->ordered_rows.size() - state->output_offset;
	const auto count = std::min<duckdb::idx_t>(remaining, kQ1OutputBatchSize);
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto &result = state->ordered_rows[state->output_offset + row];
		output.SetValue(0, row, duckdb::Value(UnpackLeft(result.key)));
		output.SetValue(1, row, duckdb::Value(UnpackRight(result.key)));
		output.SetValue(2, row, duckdb::Value::DECIMAL(result.sum_qty, kDecimalWidth38, kDecimalScale2));
		output.SetValue(3, row, duckdb::Value::DECIMAL(result.sum_base_price, kDecimalWidth38, kDecimalScale2));
		output.SetValue(4, row, duckdb::Value::DECIMAL(result.sum_disc_price, kDecimalWidth38, kDecimalScale4));
		output.SetValue(5, row, duckdb::Value::DECIMAL(result.sum_charge, kDecimalWidth38, kDecimalScale6));
		output.SetValue(6, row, duckdb::Value::DOUBLE(result.avg_qty));
		output.SetValue(7, row, duckdb::Value::DOUBLE(result.avg_price));
		output.SetValue(8, row, duckdb::Value::DOUBLE(result.avg_disc));
		output.SetValue(9, row, duckdb::Value::BIGINT(result.count_order));
	}
	output.SetCardinality(count);
	state->output_offset += count;
	return true;
}

duckdb::unique_ptr<KernelInstance> CreateQ1AuxKernel(const Q1Parameters &params) {
	return duckdb::make_uniq<Q1AuxKernel>(params);
}

} // namespace accelerator_codegen
