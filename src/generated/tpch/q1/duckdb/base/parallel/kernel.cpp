//! Q1 parallel kernel.
//! Default production path: fused parallel scan via RunParallelFilteredScan (TrustFilter).
//! Experimental chunk fallback: -DACCEL_EXPERIMENTAL_PARALLEL_Q1_OPTS + ACCEL_Q1_PARALLEL_CHUNK=1.

#include "accelerator_codegen/runtime/parallel/parallel_kernel.hpp"
#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_scale_profile.hpp"
#include "tpch/q1/duckdb/base/query.hpp"
#include "tpch/q1/duckdb/base/fused.hpp"
#include "tpch/q1/duckdb/base/parallel/aligned_agg.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
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

constexpr duckdb::idx_t kMaxQ1Groups = 6;

inline uint16_t PackGroupKeyBytes(uint8_t returnflag, uint8_t linestatus) {
	return static_cast<uint16_t>((static_cast<uint16_t>(returnflag) << 8) | linestatus);
}

inline int Q1GroupSlotFromPacked(uint16_t packed_key) {
	static std::array<int8_t, 1u << 16> slot_lut {};
	static bool initialized = false;
	if (!initialized) {
		slot_lut.fill(-1);
		slot_lut[0x4146] = 0;
		slot_lut[0x414F] = 1;
		slot_lut[0x4E46] = 2;
		slot_lut[0x4E4F] = 3;
		slot_lut[0x5246] = 4;
		slot_lut[0x524F] = 5;
		initialized = true;
	}
	const auto slot = slot_lut[packed_key];
	if (slot < 0) {
		throw duckdb::InvalidInputException("Q1 encountered unexpected returnflag/linestatus combination");
	}
	return slot;
}

inline int Q1GroupSlot(uint8_t returnflag, uint8_t linestatus) {
	return Q1GroupSlotFromPacked(PackGroupKeyBytes(returnflag, linestatus));
}

inline int Q1GroupSlot(duckdb::string_t returnflag, duckdb::string_t linestatus) {
	return Q1GroupSlotFromPacked(PackGroupKey(returnflag, linestatus));
}

inline uint16_t Q1SlotPackedKey(int slot) {
	static constexpr uint16_t kPackedKeys[kMaxQ1Groups] = {0x4146, 0x414F, 0x4E46, 0x4E4F, 0x5246, 0x524F};
	return kPackedKeys[static_cast<size_t>(slot)];
}

bool Q1ProfileEnabled() {
	if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
		return true;
	}
	return false;
}

using ProfileClock = std::chrono::steady_clock;

inline int64_t ElapsedNs(const ProfileClock::time_point &start) {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(ProfileClock::now() - start).count();
}

struct Q1WorkerProfile {
	int64_t worker_ns = 0;
	size_t chunks = 0;
	size_t surviving_rows = 0;
	size_t flat_rows = 0;
	size_t unified_rows = 0;
	size_t payload_decoded = 0;
	size_t decimal_muls = 0;
	size_t decimal_adds = 0;
	size_t unified_conversions = 0;
};

struct Q1ParallelProfile {
	int64_t total_ns = 0;
	int64_t scan_setup_ns = 0;
	int64_t native_scan_ns = 0;
	int64_t aggregate_update_ns = 0;
	int64_t reduction_ns = 0;
	int64_t average_calc_ns = 0;
	int64_t output_build_ns = 0;
	int64_t output_sort_ns = 0;
	size_t worker_count = 0;
	size_t fused_vectors = 0;
	size_t surviving_rows = 0;
	size_t flat_rows = 0;
	size_t unified_rows = 0;
	size_t payload_decoded = 0;
	size_t payload_skipped = 0;
	size_t decimal_muls = 0;
	size_t decimal_adds = 0;
	size_t unified_conversions = 0;
	size_t groups_observed = 0;
	size_t output_rows = 0;
	size_t worker_local_agg_bytes = 0;
	size_t reduction_bytes = 0;
	int64_t max_worker_ns = 0;
	int64_t avg_worker_ns = 0;
	std::vector<Q1WorkerProfile> workers;
};

void PrintQ1ParallelProfile(const Q1ParallelProfile &profile) {
	const auto pct = [&](int64_t phase_ns) {
		return profile.total_ns > 0 ? (100.0 * static_cast<double>(phase_ns) / static_cast<double>(profile.total_ns))
		                            : 0.0;
	};
	const auto max_to_avg = profile.avg_worker_ns > 0
	                            ? static_cast<double>(profile.max_worker_ns) /
	                                  static_cast<double>(profile.avg_worker_ns)
	                            : 0.0;
	std::cerr << "q1_parallel_profile total_ns=" << profile.total_ns << " scan_setup_ns=" << profile.scan_setup_ns
	          << " (" << pct(profile.scan_setup_ns) << "%)"
	          << " native_scan_ns=" << profile.native_scan_ns << " (" << pct(profile.native_scan_ns) << "%)"
	          << " aggregate_update_ns=" << profile.aggregate_update_ns << " (" << pct(profile.aggregate_update_ns)
	          << "%)"
	          << " reduction_ns=" << profile.reduction_ns << " (" << pct(profile.reduction_ns) << "%)"
	          << " average_calc_ns=" << profile.average_calc_ns << " (" << pct(profile.average_calc_ns) << "%)"
	          << " output_build_ns=" << profile.output_build_ns << " (" << pct(profile.output_build_ns) << "%)"
	          << " output_sort_ns=" << profile.output_sort_ns << " (" << pct(profile.output_sort_ns) << "%)"
	          << " worker_count=" << profile.worker_count << " fused_vectors=" << profile.fused_vectors
	          << " surviving_rows=" << profile.surviving_rows << " flat_rows=" << profile.flat_rows
	          << " unified_rows=" << profile.unified_rows << " payload_decoded=" << profile.payload_decoded
	          << " payload_skipped=0"
	          << " decimal_muls=" << profile.decimal_muls << " decimal_adds=" << profile.decimal_adds
	          << " unified_conversions=" << profile.unified_conversions << " groups_observed=" << profile.groups_observed
	          << " output_rows=" << profile.output_rows << " worker_local_agg_bytes=" << profile.worker_local_agg_bytes
	          << " reduction_bytes=" << profile.reduction_bytes << " max_worker_ns=" << profile.max_worker_ns
	          << " avg_worker_ns=" << profile.avg_worker_ns << " max_to_avg_worker=" << max_to_avg << '\n';
	for (size_t worker = 0; worker < profile.workers.size(); worker++) {
		const auto &worker_profile = profile.workers[worker];
		std::cerr << "q1_parallel_worker worker=" << worker << " worker_ns=" << worker_profile.worker_ns
		          << " chunks=" << worker_profile.chunks << " surviving_rows=" << worker_profile.surviving_rows
		          << " flat_rows=" << worker_profile.flat_rows << " unified_rows=" << worker_profile.unified_rows
		          << " payload_decoded=" << worker_profile.payload_decoded
		          << " decimal_muls=" << worker_profile.decimal_muls
		          << " decimal_adds=" << worker_profile.decimal_adds
		          << " unified_conversions=" << worker_profile.unified_conversions << '\n';
	}
}

using GroupState = Q1ParallelGroupState;

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

inline void AccumulateGroupValues(GroupState &state, int64_t quantity_raw, int64_t extendedprice_raw,
                                  int64_t discount_raw, int64_t tax_raw, Q1WorkerProfile *worker_profile) {
	AddValue(state.sum_qty, quantity_raw);
	AddValue(state.sum_base_price, extendedprice_raw);
	AddValue(state.sum_discount, discount_raw);
	state.count += 1;
	const auto one_minus_discount_raw = static_cast<int64_t>(100 - discount_raw);
	const auto one_plus_tax_raw = static_cast<int64_t>(100 + tax_raw);
	AddValue(state.sum_disc_price, extendedprice_raw * one_minus_discount_raw);
	AddValue(state.sum_charge, extendedprice_raw * one_minus_discount_raw * one_plus_tax_raw);
	if (worker_profile) {
		worker_profile->decimal_muls += 2;
		worker_profile->decimal_adds += 5;
	}
}

inline void UpdateGroupSlot(GroupState &state, int slot, std::vector<uint16_t> &used_keys, int64_t quantity_raw,
                            int64_t extendedprice_raw, int64_t discount_raw, int64_t tax_raw,
                            Q1WorkerProfile *worker_profile) {
	if (!state.used) {
		state.used = true;
		used_keys.push_back(Q1SlotPackedKey(slot));
	}
	AccumulateGroupValues(state, quantity_raw, extendedprice_raw, discount_raw, tax_raw, worker_profile);
}

struct Q1WorkerAgg {
	std::array<GroupState, kMaxQ1Groups> groups {};
	std::vector<uint16_t> used_keys;
	Q1ParallelAlignedWorkerCache aligned_cache;
	duckdb::SelectionVector composed_returnflag_sel;
	duckdb::SelectionVector composed_linestatus_sel;
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
		if (!(shipdates[sd_idx] <= cutoff_date)) {
			continue;
		}
		const auto slot = Q1GroupSlot(returnflags[rf_idx], linestatuses[ls_idx]);
		UpdateGroupSlot(agg.groups[static_cast<size_t>(slot)], static_cast<int>(slot), agg.used_keys, quantities[q_idx],
		                extendedprices[e_idx], discounts[d_idx], taxes[t_idx], nullptr);
	}
}

//! Flat + AllValid fast path: index payload with filter sel directly (TrustFilter; no shipdate residual).
void AggregateFusedFlat(duckdb::Vector &quantity_vec, duckdb::Vector &extendedprice_vec, duckdb::Vector &discount_vec,
                        duckdb::Vector &tax_vec, duckdb::Vector &returnflag_vec, duckdb::Vector &linestatus_vec,
                        duckdb::SelectionVector &sel, duckdb::idx_t count, Q1WorkerAgg &agg, Q1WorkerProfile *worker_profile,
                        int64_t *aggregate_ns_out) {
	auto *quantities = duckdb::FlatVector::GetData<int64_t>(quantity_vec);
	auto *extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vec);
	auto *discounts = duckdb::FlatVector::GetData<int64_t>(discount_vec);
	auto *taxes = duckdb::FlatVector::GetData<int64_t>(tax_vec);
	auto *returnflags = duckdb::FlatVector::GetData<duckdb::string_t>(returnflag_vec);
	auto *linestatuses = duckdb::FlatVector::GetData<duckdb::string_t>(linestatus_vec);

	const auto aggregate_start = aggregate_ns_out ? ProfileClock::now() : ProfileClock::time_point {};
	for (duckdb::idx_t i = 0; i < count; i++) {
		const auto idx = sel.get_index(i);
		const auto slot = Q1GroupSlot(returnflags[idx], linestatuses[idx]);
		UpdateGroupSlot(agg.groups[static_cast<size_t>(slot)], static_cast<int>(slot), agg.used_keys, quantities[idx],
		                extendedprices[idx], discounts[idx], taxes[idx], worker_profile);
	}
	if (worker_profile) {
		worker_profile->flat_rows += count;
		worker_profile->payload_decoded += count * 6;
	}
	if (aggregate_ns_out) {
		*aggregate_ns_out += ElapsedNs(aggregate_start);
	}
}

//! UnifiedVectorFormat path: compose filter sel with per-vector sel (ST AggregateScalarFiltered pattern).
void AggregateFusedUnified(duckdb::Vector &quantity_vec, duckdb::Vector &extendedprice_vec, duckdb::Vector &discount_vec,
                           duckdb::Vector &tax_vec, duckdb::Vector &returnflag_vec, duckdb::Vector &linestatus_vec,
                           duckdb::SelectionVector &sel, duckdb::idx_t count, duckdb::idx_t scan_count,
                           Q1WorkerAgg &agg, Q1WorkerProfile *worker_profile, int64_t *aggregate_ns_out) {
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
	if (worker_profile) {
		worker_profile->unified_conversions += 6;
	}

	auto *quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_data);
	auto *extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_data);
	auto *discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_data);
	auto *taxes = duckdb::UnifiedVectorFormat::GetData<int64_t>(tax_data);
	auto *returnflags = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(returnflag_data);
	auto *linestatuses = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(linestatus_data);

	const auto aggregate_start = aggregate_ns_out ? ProfileClock::now() : ProfileClock::time_point {};
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
		const auto slot = Q1GroupSlot(returnflags[rf_idx], linestatuses[ls_idx]);
		UpdateGroupSlot(agg.groups[static_cast<size_t>(slot)], static_cast<int>(slot), agg.used_keys, quantities[q_idx],
		                extendedprices[e_idx], discounts[d_idx], taxes[t_idx], worker_profile);
	}
	if (worker_profile) {
		worker_profile->unified_rows += count;
		worker_profile->payload_decoded += count * 6;
	}
	if (aggregate_ns_out) {
		*aggregate_ns_out += ElapsedNs(aggregate_start);
	}
}

void AccumulateQ1Fused(duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
                       duckdb::RowGroup &row_group, duckdb::SelectionVector &sel, duckdb::idx_t count,
                       Q1WorkerAgg &agg, Q1WorkerProfile *worker_profile, int64_t *native_scan_ns_out,
                       int64_t *aggregate_ns_out) {
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
	const auto scan_start = native_scan_ns_out ? ProfileClock::now() : ProfileClock::time_point {};
	const auto scan_count = row_group.ScanProjectedColumns(transaction, scan_state, kPayloadScanIndices,
	                                                       /*column_count=*/6, payload_results,
	                                                       duckdb::UpdateScanType::STANDARD);
	if (native_scan_ns_out) {
		*native_scan_ns_out += ElapsedNs(scan_start);
	}

	const auto aggregate_start = aggregate_ns_out ? ProfileClock::now() : ProfileClock::time_point {};
	if (AccumulateQ1ParallelAlignedPayload(quantity_vec, extendedprice_vec, discount_vec, tax_vec, returnflag_vec,
	                                       linestatus_vec, sel, count, scan_count, agg.groups, agg.used_keys,
	                                       agg.aligned_cache, agg.composed_returnflag_sel,
	                                       agg.composed_linestatus_sel)) {
		if (worker_profile) {
			worker_profile->surviving_rows += count;
			worker_profile->chunks += 1;
			worker_profile->flat_rows += count;
			worker_profile->payload_decoded += count * 6;
		}
		if (aggregate_ns_out) {
			*aggregate_ns_out += ElapsedNs(aggregate_start);
		}
		return;
	}

	if (IsFlatAllValid(quantity_vec) && IsFlatAllValid(extendedprice_vec) && IsFlatAllValid(discount_vec) &&
	    IsFlatAllValid(tax_vec) && IsFlatAllValid(returnflag_vec) && IsFlatAllValid(linestatus_vec)) {
		AggregateFusedFlat(quantity_vec, extendedprice_vec, discount_vec, tax_vec, returnflag_vec, linestatus_vec, sel,
		                   count, agg, worker_profile, aggregate_ns_out);
	} else {
		AggregateFusedUnified(quantity_vec, extendedprice_vec, discount_vec, tax_vec, returnflag_vec, linestatus_vec,
		                      sel, count, scan_count, agg, worker_profile, aggregate_ns_out);
	}
	if (worker_profile) {
		worker_profile->surviving_rows += count;
		worker_profile->chunks += 1;
	}
}

void MergeWorkerInto(GroupState &dst, const GroupState &src) {
	if (!src.used) {
		return;
	}
	if (!dst.used) {
		dst = src;
		return;
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
		const bool profile = Q1ProfileEnabled();
		const auto total_start = profile ? ProfileClock::now() : ProfileClock::time_point {};
		Q1ParallelProfile profile_data;
		if (profile) {
			profile_data.worker_count = ParallelWorkerCount(context);
			profile_data.workers.resize(profile_data.worker_count);
		}

		if (bound_inputs.size() != 1 || !bound_inputs[0]) {
			throw duckdb::InternalException("Q1 parallel expects one bound lineitem scan");
		}
		const auto worker_count = ParallelWorkerCount(context);
		const auto scan_setup_start = profile ? ProfileClock::now() : ProfileClock::time_point {};
		std::vector<CacheLinePadded<Q1WorkerAgg>> workers(worker_count);
		if (profile) {
			profile_data.scan_setup_ns = ElapsedNs(scan_setup_start);
			profile_data.worker_local_agg_bytes = workers.size() * sizeof(Q1WorkerAgg);
		}

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
			std::vector<int64_t> worker_elapsed_ns(worker_count, 0);
			std::vector<int64_t> worker_native_scan_ns(worker_count, 0);
			std::vector<int64_t> worker_aggregate_ns(worker_count, 0);
			RunParallelFilteredScan(
			    context, *bound_inputs[0], worker_count,
			    [&](duckdb::idx_t worker_id, duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
			        duckdb::RowGroup &row_group, duckdb::SelectionVector &sel, duckdb::idx_t count) {
				    const auto worker_start = profile ? ProfileClock::now() : ProfileClock::time_point {};
				    fused_vectors.fetch_add(1, std::memory_order_relaxed);
				    fused_rows.fetch_add(count, std::memory_order_relaxed);
				    Q1WorkerProfile *worker_profile =
				        profile ? &profile_data.workers[static_cast<size_t>(worker_id)] : nullptr;
				    AccumulateQ1Fused(transaction, scan_state, row_group, sel, count, workers[worker_id].value,
				                      worker_profile,
				                      profile ? &worker_native_scan_ns[static_cast<size_t>(worker_id)] : nullptr,
				                      profile ? &worker_aggregate_ns[static_cast<size_t>(worker_id)] : nullptr);
				    if (profile) {
					    worker_elapsed_ns[static_cast<size_t>(worker_id)] += ElapsedNs(worker_start);
				    }
			    });
			if (profile) {
				for (size_t worker = 0; worker < worker_count; worker++) {
					auto &worker_profile = profile_data.workers[worker];
					worker_profile.worker_ns = worker_elapsed_ns[worker];
					profile_data.native_scan_ns += worker_native_scan_ns[worker];
					profile_data.aggregate_update_ns += worker_aggregate_ns[worker];
					profile_data.surviving_rows += worker_profile.surviving_rows;
					profile_data.flat_rows += worker_profile.flat_rows;
					profile_data.unified_rows += worker_profile.unified_rows;
					profile_data.payload_decoded += worker_profile.payload_decoded;
					profile_data.decimal_muls += worker_profile.decimal_muls;
					profile_data.decimal_adds += worker_profile.decimal_adds;
					profile_data.unified_conversions += worker_profile.unified_conversions;
					profile_data.fused_vectors += worker_profile.chunks;
					profile_data.max_worker_ns = std::max(profile_data.max_worker_ns, worker_profile.worker_ns);
					profile_data.avg_worker_ns += worker_profile.worker_ns;
				}
				if (worker_count > 0) {
					profile_data.avg_worker_ns /= static_cast<int64_t>(worker_count);
				}
			}
		}

		const auto reduction_start = profile ? ProfileClock::now() : ProfileClock::time_point {};
		merged_groups = std::array<GroupState, kMaxQ1Groups> {};
		merged_keys.clear();
		for (const auto &worker : workers) {
			for (duckdb::idx_t slot = 0; slot < kMaxQ1Groups; slot++) {
				const auto &src = worker.value.groups[slot];
				if (!src.used) {
					continue;
				}
				auto &dst = merged_groups[slot];
				if (!dst.used) {
					dst.used = true;
					merged_keys.push_back(Q1SlotPackedKey(static_cast<int>(slot)));
				}
				MergeWorkerInto(dst, src);
			}
		}
		if (profile) {
			profile_data.reduction_ns = ElapsedNs(reduction_start);
			profile_data.groups_observed = merged_keys.size();
			profile_data.reduction_bytes = merged_keys.size() * sizeof(GroupState);
			profile_total_start_ = total_start;
		}
		profile_snapshot = profile ? std::make_optional(profile_data) : std::nullopt;
		parallel_complete_ = true;
	}

	void Finalize() override {
		if (finalized) {
			return;
		}
		if (!parallel_complete_) {
			throw duckdb::InternalException("Q1ParallelKernel::Finalize called before RunParallel");
		}
		const bool profile = Q1ProfileEnabled();
		const auto output_build_start = profile ? ProfileClock::now() : ProfileClock::time_point {};
		ordered_rows.reserve(merged_keys.size());
		int64_t average_calc_ns = 0;
		for (const auto key : merged_keys) {
			const auto average_start = profile ? ProfileClock::now() : ProfileClock::time_point {};
			int slot = -1;
			for (duckdb::idx_t candidate = 0; candidate < kMaxQ1Groups; candidate++) {
				if (Q1SlotPackedKey(static_cast<int>(candidate)) == key) {
					slot = static_cast<int>(candidate);
					break;
				}
			}
			if (slot < 0) {
				throw duckdb::InternalException("Q1 parallel finalize missing slot for merged key");
			}
			auto &group = merged_groups[static_cast<size_t>(slot)];
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
			if (profile) {
				average_calc_ns += ElapsedNs(average_start);
			}
		}
		const auto output_build_ns = profile ? ElapsedNs(output_build_start) : 0;
		const auto output_sort_start = profile ? ProfileClock::now() : ProfileClock::time_point {};
		std::sort(ordered_rows.begin(), ordered_rows.end(),
		          [](const OutputRow &lhs, const OutputRow &rhs) { return lhs.key < rhs.key; });
		if (profile && profile_snapshot) {
			profile_snapshot->output_build_ns = output_build_ns;
			profile_snapshot->average_calc_ns = average_calc_ns;
			profile_snapshot->output_sort_ns = ElapsedNs(output_sort_start);
			profile_snapshot->output_rows = ordered_rows.size();
			profile_snapshot->total_ns = ElapsedNs(profile_total_start_);
			PrintQ1ParallelProfile(*profile_snapshot);
			ParallelScaleProfile scale;
			scale.query = "Q1";
			scale.worker_count = profile_snapshot->worker_count;
			scale.total_ns = profile_snapshot->total_ns;
			scale.scan.rows_scanned = profile_snapshot->surviving_rows;
			scale.scan.rows_after_local_predicate = profile_snapshot->surviving_rows;
			scale.scan.rows_decoded = profile_snapshot->payload_decoded;
			scale.state.logical_elements = kMaxQ1Groups;
			scale.state.element_width_bytes = sizeof(GroupState);
			scale.state.logical_bytes = kMaxQ1Groups * sizeof(GroupState);
			scale.state.allocated_bytes = profile_snapshot->worker_local_agg_bytes;
			scale.worker_replication.per_worker_bytes = profile_snapshot->worker_local_agg_bytes / std::max<size_t>(1, profile_snapshot->worker_count);
			scale.worker_replication.worker_count = profile_snapshot->worker_count;
			scale.worker_replication.replication_kind = "fixed";
			UpdateWorkerReplicationTotals(scale.worker_replication);
			scale.intermediate.records_materialized = profile_snapshot->output_rows;
			scale.merge.serial_merge_ns = profile_snapshot->reduction_ns;
			scale.merge.final_output_cardinality = profile_snapshot->output_rows;
			scale.phase_ns = {{"scan_setup", profile_snapshot->scan_setup_ns},
			                  {"native_scan", profile_snapshot->native_scan_ns},
			                  {"aggregate", profile_snapshot->aggregate_update_ns},
			                  {"reduction", profile_snapshot->reduction_ns},
			                  {"output_sort", profile_snapshot->output_sort_ns}};
			scale.balance.max_to_avg_time = profile_snapshot->max_worker_ns > 0 && profile_snapshot->avg_worker_ns > 0
			                                    ? static_cast<double>(profile_snapshot->max_worker_ns) /
			                                          static_cast<double>(profile_snapshot->avg_worker_ns)
			                                    : 0.0;
			EmitParallelScaleProfile(scale);
		}
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
	std::array<GroupState, kMaxQ1Groups> merged_groups {};
	std::vector<uint16_t> merged_keys;
	std::vector<OutputRow> ordered_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
	bool parallel_complete_ = false;
	bool used_fused = false;
	std::atomic<duckdb::idx_t> fused_vectors {0};
	std::atomic<duckdb::idx_t> fused_rows {0};
	std::optional<Q1ParallelProfile> profile_snapshot;
	ProfileClock::time_point profile_total_start_ {};
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ1ParallelKernel(const Q1Parameters &params) {
	return duckdb::make_uniq<Q1ParallelKernel>(params);
}

} // namespace accelerator_codegen
