//! Q1 parallel kernel.
//! Default production path: fused parallel scan via RunParallelFilteredScan (TrustFilter).
//! Experimental chunk fallback: -DACCEL_EXPERIMENTAL_PARALLEL_Q1_OPTS + ACCEL_Q1_PARALLEL_CHUNK=1.

#include "accelerator_codegen/parallel/parallel_kernel.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/tpch_q1.hpp"
#include "accelerator_codegen/tpch_q1_fused.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/table/filtered_scan_consumer.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kDecimalScale2 = 2;
constexpr uint8_t kDecimalScale4 = 4;
constexpr uint8_t kDecimalScale6 = 6;
constexpr uint8_t kDecimalWidth38 = 38;
constexpr duckdb::idx_t kQ1OutputBatchSize = 2;
constexpr duckdb::idx_t kProjectedColumnCount = 7;
constexpr duckdb::idx_t kQuantityScanIndex = 1;
constexpr duckdb::idx_t kExtendedpriceScanIndex = 2;
constexpr duckdb::idx_t kDiscountScanIndex = 3;
constexpr duckdb::idx_t kTaxScanIndex = 4;
constexpr duckdb::idx_t kReturnflagScanIndex = 5;
constexpr duckdb::idx_t kLinestatusScanIndex = 6;

#if defined(__SIZEOF_INT128__)
using AccT = __int128_t;

duckdb::hugeint_t FromAccT(AccT value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}

inline void AddValue(AccT &accumulator, int64_t value) {
	accumulator += static_cast<AccT>(value);
}
#else
using AccT = duckdb::hugeint_t;

duckdb::hugeint_t FromAccT(AccT value) {
	return value;
}

inline void AddValue(AccT &accumulator, int64_t input) {
	auto addend = duckdb::Hugeint::Convert(input);
	if (!duckdb::Hugeint::TryAddInPlace(accumulator, addend)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q1 parallel aggregate");
	}
}
#endif

double HugeintScaleToDouble(AccT value, uint8_t scale, uint64_t count) {
	auto as_hugeint = FromAccT(value);
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

struct GroupState {
	AccT sum_qty = 0;
	AccT sum_base_price = 0;
	AccT sum_disc_price = 0;
	AccT sum_charge = 0;
	AccT sum_discount = 0;
	uint64_t count = 0;
	bool used = false;
};

struct OutputRow {
	uint16_t key;
	duckdb::hugeint_t sum_qty;
	duckdb::hugeint_t sum_base_price;
	duckdb::hugeint_t sum_disc_price;
	duckdb::hugeint_t sum_charge;
	double avg_qty;
	double avg_price;
	double avg_disc;
	int64_t count_order;
};

inline void UpdateGroup(GroupState &state, std::vector<uint16_t> &used_keys, uint16_t group_key, int64_t quantity_raw,
                        int64_t extendedprice_raw, int64_t discount_raw, int64_t tax_raw) {
	if (!state.used) {
		state.used = true;
		used_keys.push_back(group_key);
	}
	AddValue(state.sum_qty, quantity_raw);
	AddValue(state.sum_base_price, extendedprice_raw);
	AddValue(state.sum_discount, discount_raw);
	state.count += 1;
	// Match q1_fused UpdateChunkLocalState<false>: DECIMAL(15,2) raw int64 scaled by 100.
	// disc_price raw = ep * (100 - disc)           // scale 4
	// charge raw     = ep * (100 - disc) * (100 + tax)  // scale 6
	const auto one_minus_discount_raw = static_cast<int64_t>(100 - discount_raw);
	const auto one_plus_tax_raw = static_cast<int64_t>(100 + tax_raw);
	AddValue(state.sum_disc_price, extendedprice_raw * one_minus_discount_raw);
	AddValue(state.sum_charge, extendedprice_raw * one_minus_discount_raw * one_plus_tax_raw);
}

struct Q1WorkerAgg {
	std::unique_ptr<std::array<GroupState, 1u << 16>> groups =
	    std::make_unique<std::array<GroupState, 1u << 16>>();
	std::vector<uint16_t> used_keys;
};

inline bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR && duckdb::FlatVector::Validity(vector).AllValid();
}

#ifdef ACCEL_EXPERIMENTAL_PARALLEL_Q1_OPTS
bool UseExperimentalChunkPath() {
	const char *value = std::getenv("ACCEL_Q1_PARALLEL_CHUNK");
	if (!value || !*value) {
		return false;
	}
	return value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y';
}
#endif

void AccumulateQ1Chunk(duckdb::DataChunk &chunk, duckdb::date_t cutoff_date, Q1WorkerAgg &agg) {
	auto &shipdate_vector = chunk.data[Q1LineitemChunkLayout::SHIPDATE_INDEX];
	auto &quantity_vector = chunk.data[Q1LineitemChunkLayout::QUANTITY_INDEX];
	auto &extendedprice_vector = chunk.data[Q1LineitemChunkLayout::EXTENDEDPRICE_INDEX];
	auto &discount_vector = chunk.data[Q1LineitemChunkLayout::DISCOUNT_INDEX];
	auto &tax_vector = chunk.data[Q1LineitemChunkLayout::TAX_INDEX];
	auto &returnflag_vector = chunk.data[Q1LineitemChunkLayout::RETURNFLAG_INDEX];
	auto &linestatus_vector = chunk.data[Q1LineitemChunkLayout::LINESTATUS_INDEX];

	duckdb::UnifiedVectorFormat shipdate_data;
	duckdb::UnifiedVectorFormat quantity_data;
	duckdb::UnifiedVectorFormat extendedprice_data;
	duckdb::UnifiedVectorFormat discount_data;
	duckdb::UnifiedVectorFormat tax_data;
	duckdb::UnifiedVectorFormat returnflag_data;
	duckdb::UnifiedVectorFormat linestatus_data;
	shipdate_vector.ToUnifiedFormat(chunk.size(), shipdate_data);
	quantity_vector.ToUnifiedFormat(chunk.size(), quantity_data);
	extendedprice_vector.ToUnifiedFormat(chunk.size(), extendedprice_data);
	discount_vector.ToUnifiedFormat(chunk.size(), discount_data);
	tax_vector.ToUnifiedFormat(chunk.size(), tax_data);
	returnflag_vector.ToUnifiedFormat(chunk.size(), returnflag_data);
	linestatus_vector.ToUnifiedFormat(chunk.size(), linestatus_data);

	auto *shipdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdate_data);
	auto *quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_data);
	auto *extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_data);
	auto *discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_data);
	auto *taxes = duckdb::UnifiedVectorFormat::GetData<int64_t>(tax_data);
	auto *returnflags = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(returnflag_data);
	auto *linestatuses = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(linestatus_data);

	auto &groups = *agg.groups;
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto sd_idx = shipdate_data.sel->get_index(row);
		const auto q_idx = quantity_data.sel->get_index(row);
		const auto e_idx = extendedprice_data.sel->get_index(row);
		const auto d_idx = discount_data.sel->get_index(row);
		const auto t_idx = tax_data.sel->get_index(row);
		const auto rf_idx = returnflag_data.sel->get_index(row);
		const auto ls_idx = linestatus_data.sel->get_index(row);
		if (!shipdate_data.validity.RowIsValid(sd_idx) || !quantity_data.validity.RowIsValid(q_idx) ||
		    !extendedprice_data.validity.RowIsValid(e_idx) || !discount_data.validity.RowIsValid(d_idx) ||
		    !tax_data.validity.RowIsValid(t_idx) || !returnflag_data.validity.RowIsValid(rf_idx) ||
		    !linestatus_data.validity.RowIsValid(ls_idx)) {
			continue;
		}
		// Residual shipdate check (filters are pushed; still verify for safety).
		if (!(shipdates[sd_idx] <= cutoff_date)) {
			continue;
		}
		const auto key = PackGroupKey(returnflags[rf_idx], linestatuses[ls_idx]);
		UpdateGroup(groups[key], agg.used_keys, key, quantities[q_idx], extendedprices[e_idx], discounts[d_idx],
		            taxes[t_idx]);
	}
}

//! Flat + AllValid fast path: index payload with filter sel directly (TrustFilter; no shipdate residual).
void AggregateFusedFlat(duckdb::Vector &quantity_vec, duckdb::Vector &extendedprice_vec, duckdb::Vector &discount_vec,
                        duckdb::Vector &tax_vec, duckdb::Vector &returnflag_vec, duckdb::Vector &linestatus_vec,
                        duckdb::SelectionVector &sel, duckdb::idx_t count, Q1WorkerAgg &agg) {
	auto *quantities = duckdb::FlatVector::GetData<int64_t>(quantity_vec);
	auto *extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vec);
	auto *discounts = duckdb::FlatVector::GetData<int64_t>(discount_vec);
	auto *taxes = duckdb::FlatVector::GetData<int64_t>(tax_vec);
	auto *returnflags = duckdb::FlatVector::GetData<duckdb::string_t>(returnflag_vec);
	auto *linestatuses = duckdb::FlatVector::GetData<duckdb::string_t>(linestatus_vec);

	auto &groups = *agg.groups;
	for (duckdb::idx_t i = 0; i < count; i++) {
		const auto idx = sel.get_index(i);
		const auto key = PackGroupKey(returnflags[idx], linestatuses[idx]);
		UpdateGroup(groups[key], agg.used_keys, key, quantities[idx], extendedprices[idx], discounts[idx], taxes[idx]);
	}
}

//! UnifiedVectorFormat path: compose filter sel with per-vector sel (ST AggregateScalarFiltered pattern).
void AggregateFusedUnified(duckdb::Vector &quantity_vec, duckdb::Vector &extendedprice_vec, duckdb::Vector &discount_vec,
                           duckdb::Vector &tax_vec, duckdb::Vector &returnflag_vec, duckdb::Vector &linestatus_vec,
                           duckdb::SelectionVector &sel, duckdb::idx_t count, duckdb::idx_t scan_count,
                           Q1WorkerAgg &agg) {
	duckdb::UnifiedVectorFormat quantity_data;
	duckdb::UnifiedVectorFormat extendedprice_data;
	duckdb::UnifiedVectorFormat discount_data;
	duckdb::UnifiedVectorFormat tax_data;
	duckdb::UnifiedVectorFormat returnflag_data;
	duckdb::UnifiedVectorFormat linestatus_data;
	quantity_vec.ToUnifiedFormat(scan_count, quantity_data);
	extendedprice_vec.ToUnifiedFormat(scan_count, extendedprice_data);
	discount_vec.ToUnifiedFormat(scan_count, discount_data);
	tax_vec.ToUnifiedFormat(scan_count, tax_data);
	returnflag_vec.ToUnifiedFormat(scan_count, returnflag_data);
	linestatus_vec.ToUnifiedFormat(scan_count, linestatus_data);

	auto *quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_data);
	auto *extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_data);
	auto *discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_data);
	auto *taxes = duckdb::UnifiedVectorFormat::GetData<int64_t>(tax_data);
	auto *returnflags = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(returnflag_data);
	auto *linestatuses = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(linestatus_data);

	auto &groups = *agg.groups;
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto base = sel.get_index(row);
		const auto q_idx = quantity_data.sel->get_index(base);
		const auto e_idx = extendedprice_data.sel->get_index(base);
		const auto d_idx = discount_data.sel->get_index(base);
		const auto t_idx = tax_data.sel->get_index(base);
		const auto rf_idx = returnflag_data.sel->get_index(base);
		const auto ls_idx = linestatus_data.sel->get_index(base);
		if (!quantity_data.validity.RowIsValid(q_idx) || !extendedprice_data.validity.RowIsValid(e_idx) ||
		    !discount_data.validity.RowIsValid(d_idx) || !tax_data.validity.RowIsValid(t_idx) ||
		    !returnflag_data.validity.RowIsValid(rf_idx) || !linestatus_data.validity.RowIsValid(ls_idx)) {
			continue;
		}
		const auto key = PackGroupKey(returnflags[rf_idx], linestatuses[ls_idx]);
		UpdateGroup(groups[key], agg.used_keys, key, quantities[q_idx], extendedprices[e_idx], discounts[d_idx],
		            taxes[t_idx]);
	}
}

void AccumulateQ1Fused(duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
                       duckdb::RowGroup &row_group, duckdb::SelectionVector &sel, duckdb::idx_t count,
                       Q1WorkerAgg &agg) {
	const auto &column_ids = scan_state.GetColumnIds();
	if (column_ids.size() != kProjectedColumnCount) {
		throw duckdb::InternalException("Q1 parallel fused expected %llu projected columns, got %llu",
		                                static_cast<unsigned long long>(kProjectedColumnCount),
		                                static_cast<unsigned long long>(column_ids.size()));
	}

	static constexpr duckdb::idx_t kPayloadScanIndices[] = {
	    kQuantityScanIndex, kExtendedpriceScanIndex, kDiscountScanIndex,
	    kTaxScanIndex,      kReturnflagScanIndex,    kLinestatusScanIndex,
	};

	duckdb::Vector quantity_vec(duckdb::LogicalType::DECIMAL(15, 2));
	duckdb::Vector extendedprice_vec(duckdb::LogicalType::DECIMAL(15, 2));
	duckdb::Vector discount_vec(duckdb::LogicalType::DECIMAL(15, 2));
	duckdb::Vector tax_vec(duckdb::LogicalType::DECIMAL(15, 2));
	duckdb::Vector returnflag_vec(duckdb::LogicalType::VARCHAR);
	duckdb::Vector linestatus_vec(duckdb::LogicalType::VARCHAR);

	duckdb::Vector *payload_results[] = {&quantity_vec, &extendedprice_vec, &discount_vec,
	                                     &tax_vec,      &returnflag_vec,    &linestatus_vec};
	const auto scan_count = row_group.ScanProjectedColumns(transaction, scan_state, kPayloadScanIndices,
	                                                       /*column_count=*/6, payload_results,
	                                                       duckdb::UpdateScanType::STANDARD);

	if (IsFlatAllValid(quantity_vec) && IsFlatAllValid(extendedprice_vec) && IsFlatAllValid(discount_vec) &&
	    IsFlatAllValid(tax_vec) && IsFlatAllValid(returnflag_vec) && IsFlatAllValid(linestatus_vec)) {
		AggregateFusedFlat(quantity_vec, extendedprice_vec, discount_vec, tax_vec, returnflag_vec, linestatus_vec, sel,
		                   count, agg);
	} else {
		AggregateFusedUnified(quantity_vec, extendedprice_vec, discount_vec, tax_vec, returnflag_vec, linestatus_vec,
		                      sel, count, scan_count, agg);
	}
}

void MergeWorkerInto(GroupState &dst, std::vector<uint16_t> &merged_keys, uint16_t key, const GroupState &src) {
	if (!src.used) {
		return;
	}
	if (!dst.used) {
		dst.used = true;
		merged_keys.push_back(key);
	}
#if defined(__SIZEOF_INT128__)
	dst.sum_qty += src.sum_qty;
	dst.sum_base_price += src.sum_base_price;
	dst.sum_disc_price += src.sum_disc_price;
	dst.sum_charge += src.sum_charge;
	dst.sum_discount += src.sum_discount;
#else
	if (!duckdb::Hugeint::TryAddInPlace(dst.sum_qty, src.sum_qty) ||
	    !duckdb::Hugeint::TryAddInPlace(dst.sum_base_price, src.sum_base_price) ||
	    !duckdb::Hugeint::TryAddInPlace(dst.sum_disc_price, src.sum_disc_price) ||
	    !duckdb::Hugeint::TryAddInPlace(dst.sum_charge, src.sum_charge) ||
	    !duckdb::Hugeint::TryAddInPlace(dst.sum_discount, src.sum_discount)) {
		throw duckdb::OutOfRangeException("Overflow while merging Q1 parallel aggregates");
	}
#endif
	dst.count += src.count;
}

class Q1ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q1ParallelKernel(Q1Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 1 || !bound_inputs[0]) {
			throw duckdb::InternalException("Q1 parallel expects one bound lineitem scan");
		}
		const auto worker_count = ParallelWorkerCount(context);
		std::vector<CacheLinePadded<Q1WorkerAgg>> workers(worker_count);

		bool use_chunk = false;
#ifdef ACCEL_EXPERIMENTAL_PARALLEL_Q1_OPTS
		use_chunk = UseExperimentalChunkPath();
#endif
		if (use_chunk) {
			used_fused = false;
			RunParallelChunkScan(context, *bound_inputs[0], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
				                     AccumulateQ1Chunk(chunk, params.cutoff_date, workers[worker_id].value);
			                     });
		} else {
			if (!NativeScanHasFilters(*bound_inputs[0])) {
				throw duckdb::InvalidInputException(
				    "Q1 parallel fused scan requires a pushed shipdate filter; refusing TrustFilter without native "
				    "filters");
			}
			used_fused = true;
			fused_vectors = 0;
			fused_rows = 0;
			RunParallelFilteredScan(
			    context, *bound_inputs[0], worker_count,
			    [&](duckdb::idx_t worker_id, duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
			        duckdb::RowGroup &row_group, duckdb::SelectionVector &sel, duckdb::idx_t count) {
				    fused_vectors.fetch_add(1, std::memory_order_relaxed);
				    fused_rows.fetch_add(count, std::memory_order_relaxed);
				    AccumulateQ1Fused(transaction, scan_state, row_group, sel, count, workers[worker_id].value);
			    });
		}

		merged_groups = std::make_unique<std::array<GroupState, 1u << 16>>();
		merged_keys.clear();
		for (const auto &worker : workers) {
			for (const auto key : worker.value.used_keys) {
				MergeWorkerInto((*merged_groups)[key], merged_keys, key, (*worker.value.groups)[key]);
			}
		}
	}

	void Finalize() override {
		if (finalized) {
			return;
		}
		if (!merged_groups) {
			throw duckdb::InternalException("Q1ParallelKernel::Finalize called before RunParallel");
		}
		ordered_rows.reserve(merged_keys.size());
		for (const auto key : merged_keys) {
			auto &group = (*merged_groups)[key];
			OutputRow row {key,
			               FromAccT(group.sum_qty),
			               FromAccT(group.sum_base_price),
			               FromAccT(group.sum_disc_price),
			               FromAccT(group.sum_charge),
			               HugeintScaleToDouble(group.sum_qty, kDecimalScale2, group.count),
			               HugeintScaleToDouble(group.sum_base_price, kDecimalScale2, group.count),
			               HugeintScaleToDouble(group.sum_discount, kDecimalScale2, group.count),
			               static_cast<int64_t>(group.count)};
			ordered_rows.push_back(row);
		}
		std::sort(ordered_rows.begin(), ordered_rows.end(),
		          [](const OutputRow &lhs, const OutputRow &rhs) { return lhs.key < rhs.key; });
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || output_offset >= ordered_rows.size()) {
			return false;
		}
		const auto remaining = ordered_rows.size() - output_offset;
		const auto count = std::min<duckdb::idx_t>(remaining, kQ1OutputBatchSize);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto &result = ordered_rows[output_offset + row];
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
		output_offset += count;
		return true;
	}

private:
	Q1Parameters params;
	std::unique_ptr<std::array<GroupState, 1u << 16>> merged_groups;
	std::vector<uint16_t> merged_keys;
	std::vector<OutputRow> ordered_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
	bool used_fused = false;
	std::atomic<duckdb::idx_t> fused_vectors {0};
	std::atomic<duckdb::idx_t> fused_rows {0};
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ1ParallelKernel(const Q1Parameters &params) {
	return duckdb::make_uniq<Q1ParallelKernel>(params);
}

} // namespace accelerator_codegen
