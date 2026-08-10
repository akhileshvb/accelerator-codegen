#include "tpch/q1/base/parallel/aligned_agg.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kProjectedColumnCount = 7;
constexpr duckdb::idx_t kQuantityScanIndex = 1;
constexpr duckdb::idx_t kExtendedpriceScanIndex = 2;
constexpr duckdb::idx_t kDiscountScanIndex = 3;
constexpr duckdb::idx_t kTaxScanIndex = 4;
constexpr duckdb::idx_t kReturnflagScanIndex = 5;
constexpr duckdb::idx_t kLinestatusScanIndex = 6;
constexpr duckdb::idx_t kMaxQ1Groups = 6;
constexpr duckdb::idx_t kMaxFixedSlotBanks = 8;
constexpr uint8_t kInvalidSlot = 0xFF;

using AccT = Q1ParallelGroupState::AccT;

#if defined(__SIZEOF_INT128__)
inline void AddValue(AccT &accumulator, int64_t value) {
	accumulator += static_cast<AccT>(value);
}
#else
inline void AddValue(AccT &accumulator, int64_t input) {
	auto addend = duckdb::Hugeint::Convert(input);
	if (!duckdb::Hugeint::TryAddInPlace(accumulator, addend)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q1 parallel aligned aggregate");
	}
}
#endif

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
		return -1;
	}
	return slot;
}

inline uint16_t Q1SlotPackedKey(int slot) {
	static constexpr uint16_t kPackedKeys[kMaxQ1Groups] = {0x4146, 0x414F, 0x4E46, 0x4E4F, 0x5246, 0x524F};
	return kPackedKeys[static_cast<size_t>(slot)];
}

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR && duckdb::FlatVector::Validity(vector).AllValid();
}

enum class AlignedIndexMode : uint8_t {
	IdentityFilter = 0,
	Nested = 1,
	Composed = 2,
};

struct AlignedAggView {
	AlignedIndexMode index_mode = AlignedIndexMode::Nested;
	const duckdb::SelectionVector *filter_sel = nullptr;
	const duckdb::SelectionVector *returnflag_dict_sel = nullptr;
	const duckdb::SelectionVector *linestatus_dict_sel = nullptr;
	const duckdb::SelectionVector *composed_returnflag_sel = nullptr;
	const duckdb::SelectionVector *composed_linestatus_sel = nullptr;
	const int64_t *quantities = nullptr;
	const int64_t *extendedprices = nullptr;
	const int64_t *discounts = nullptr;
	const int64_t *taxes = nullptr;
	const duckdb::string_t *returnflags = nullptr;
	const duckdb::string_t *linestatuses = nullptr;
	const duckdb::ValidityMask *returnflag_validity = nullptr;
	const duckdb::ValidityMask *linestatus_validity = nullptr;
	bool returnflag_all_valid = false;
	bool linestatus_all_valid = false;
};

struct DominantStringByteCache {
	duckdb::idx_t returnflag_dictionary_size = 0;
	duckdb::idx_t linestatus_dictionary_size = 0;
	std::vector<uint8_t> returnflag_chars;
	std::vector<uint8_t> linestatus_chars;
};

bool BuildByteCacheFromAlignedView(duckdb::Vector &returnflag_vec, duckdb::Vector &linestatus_vec,
                                   const AlignedAggView &view, duckdb::idx_t count, DominantStringByteCache &cache,
                                   std::vector<uint8_t> &returnflag_seen, std::vector<uint8_t> &linestatus_seen);

bool RebuildAlignedSlotCache(duckdb::Vector &returnflag_vec, duckdb::Vector &linestatus_vec, const AlignedAggView &view,
                             duckdb::idx_t count, Q1ParallelAlignedWorkerCache &cache) {
	const auto rf_dict_size = duckdb::DictionaryVector::DictionarySize(returnflag_vec);
	const auto ls_dict_size = duckdb::DictionaryVector::DictionarySize(linestatus_vec);
	if (!rf_dict_size.IsValid() || !ls_dict_size.IsValid()) {
		cache.valid = false;
		return false;
	}
	cache.returnflag_dictionary_size = rf_dict_size.GetIndex();
	cache.linestatus_dictionary_size = ls_dict_size.GetIndex();
	cache.returnflag_chars.assign(cache.returnflag_dictionary_size, 0);
	cache.linestatus_chars.assign(cache.linestatus_dictionary_size, 0);
	for (duckdb::idx_t index = 0; index < cache.returnflag_dictionary_size; index++) {
		if (!view.returnflag_all_valid && !view.returnflag_validity->RowIsValidUnsafe(index)) {
			cache.valid = false;
			return false;
		}
		const auto returnflag = view.returnflags[index];
		if (returnflag.GetSize() != 1) {
			cache.valid = false;
			return false;
		}
		cache.returnflag_chars[index] = static_cast<uint8_t>(returnflag.GetDataUnsafe()[0]);
	}
	for (duckdb::idx_t index = 0; index < cache.linestatus_dictionary_size; index++) {
		if (!view.linestatus_all_valid && !view.linestatus_validity->RowIsValidUnsafe(index)) {
			cache.valid = false;
			return false;
		}
		const auto linestatus = view.linestatuses[index];
		if (linestatus.GetSize() != 1) {
			cache.valid = false;
			return false;
		}
		cache.linestatus_chars[index] = static_cast<uint8_t>(linestatus.GetDataUnsafe()[0]);
	}
	cache.returnflag_child = view.returnflags;
	cache.linestatus_child = view.linestatuses;
	cache.returnflag_validity = view.returnflag_validity;
	cache.linestatus_validity = view.linestatus_validity;
	cache.returnflag_all_valid = view.returnflag_all_valid;
	cache.linestatus_all_valid = view.linestatus_all_valid;

	const auto combo_count = cache.returnflag_dictionary_size * cache.linestatus_dictionary_size;
	if (combo_count > 256) {
		cache.valid = false;
		return false;
	}
	cache.pair_to_slot.assign(combo_count, kInvalidSlot);
	for (duckdb::idx_t returnflag_index = 0; returnflag_index < cache.returnflag_dictionary_size; returnflag_index++) {
		const auto returnflag_char = cache.returnflag_chars[returnflag_index];
		const auto base = returnflag_index * cache.linestatus_dictionary_size;
		for (duckdb::idx_t linestatus_index = 0; linestatus_index < cache.linestatus_dictionary_size;
		     linestatus_index++) {
			const auto packed =
			    PackGroupKeyBytes(returnflag_char, cache.linestatus_chars[linestatus_index]);
			const auto slot = Q1GroupSlotFromPacked(packed);
			if (slot >= 0) {
				cache.pair_to_slot[base + linestatus_index] = static_cast<uint8_t>(slot);
			}
		}
	}
	cache.valid = true;
	return true;
}

bool CanReuseAlignedSlotCache(const AlignedAggView &view, const Q1ParallelAlignedWorkerCache &cache,
                              duckdb::Vector &returnflag_vec, duckdb::Vector &linestatus_vec) {
	if (!cache.valid) {
		return false;
	}
	const auto rf_size = duckdb::DictionaryVector::DictionarySize(returnflag_vec);
	const auto ls_size = duckdb::DictionaryVector::DictionarySize(linestatus_vec);
	if (!rf_size.IsValid() || !ls_size.IsValid()) {
		return false;
	}
	return cache.returnflag_child == view.returnflags && cache.linestatus_child == view.linestatuses &&
	       cache.returnflag_validity == view.returnflag_validity &&
	       cache.linestatus_validity == view.linestatus_validity &&
	       cache.returnflag_all_valid == view.returnflag_all_valid &&
	       cache.linestatus_all_valid == view.linestatus_all_valid &&
	       cache.returnflag_dictionary_size == rf_size.GetIndex() &&
	       cache.linestatus_dictionary_size == ls_size.GetIndex();
}

void ResetBankStates(Q1ParallelAlignedWorkerCache &cache) {
	for (auto &bank : cache.bank_states) {
		for (auto &slot_state : bank) {
			slot_state = {};
		}
	}
}

void ComposeDictionarySel(const duckdb::SelectionVector &filter_sel, const duckdb::SelectionVector &dict_sel,
                          duckdb::idx_t count, duckdb::SelectionVector &composed) {
	for (duckdb::idx_t row = 0; row < count; row++) {
		composed.set_index(row, dict_sel.get_index(filter_sel.get_index(row)));
	}
}

bool FilterSelIsIdentity(const duckdb::SelectionVector &filter_sel) {
	return !filter_sel.IsSet();
}

bool FilterSelIsDensePrefix(const duckdb::SelectionVector &filter_sel, duckdb::idx_t count, duckdb::idx_t scan_count) {
	if (count != scan_count) {
		return false;
	}
	if (FilterSelIsIdentity(filter_sel)) {
		return true;
	}
	for (duckdb::idx_t row = 0; row < count; row++) {
		if (filter_sel.get_index(row) != row) {
			return false;
		}
	}
	return true;
}

bool BindAlignedDictString(duckdb::Vector &vector, const duckdb::string_t *&out_data,
                           const duckdb::ValidityMask *&out_validity, bool &out_all_valid,
                           const duckdb::SelectionVector *&out_dict_sel) {
	if (vector.GetVectorType() != duckdb::VectorType::DICTIONARY_VECTOR) {
		return false;
	}
	auto &child = duckdb::DictionaryVector::Child(vector);
	if (child.GetVectorType() != duckdb::VectorType::FLAT_VECTOR) {
		return false;
	}
	out_dict_sel = &duckdb::DictionaryVector::SelVector(vector);
	out_data = duckdb::FlatVector::GetData<duckdb::string_t>(child);
	out_validity = &duckdb::FlatVector::Validity(child);
	out_all_valid = out_validity->AllValid();
	return true;
}

AlignedIndexMode ChooseAlignedIndexMode(const duckdb::SelectionVector &filter_sel, duckdb::idx_t count,
                                        duckdb::idx_t scan_count) {
	if (FilterSelIsIdentity(filter_sel) || FilterSelIsDensePrefix(filter_sel, count, scan_count)) {
		return AlignedIndexMode::IdentityFilter;
	}
	return AlignedIndexMode::Nested;
}

bool TryGetAlignedAggView(duckdb::Vector &quantity_vec, duckdb::Vector &extendedprice_vec, duckdb::Vector &discount_vec,
                          duckdb::Vector &tax_vec, duckdb::Vector &returnflag_vec, duckdb::Vector &linestatus_vec,
                          duckdb::SelectionVector &filter_sel, duckdb::idx_t count, duckdb::idx_t scan_count,
                          duckdb::SelectionVector &composed_returnflag_sel,
                          duckdb::SelectionVector &composed_linestatus_sel, AlignedAggView &view) {
	if (!IsFlatAllValid(quantity_vec) || !IsFlatAllValid(extendedprice_vec) || !IsFlatAllValid(discount_vec) ||
	    !IsFlatAllValid(tax_vec)) {
		return false;
	}
	if (!BindAlignedDictString(returnflag_vec, view.returnflags, view.returnflag_validity, view.returnflag_all_valid,
	                           view.returnflag_dict_sel)) {
		return false;
	}
	if (!BindAlignedDictString(linestatus_vec, view.linestatuses, view.linestatus_validity, view.linestatus_all_valid,
	                           view.linestatus_dict_sel)) {
		return false;
	}

	view.filter_sel = &filter_sel;
	view.quantities = duckdb::FlatVector::GetData<int64_t>(quantity_vec);
	view.extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vec);
	view.discounts = duckdb::FlatVector::GetData<int64_t>(discount_vec);
	view.taxes = duckdb::FlatVector::GetData<int64_t>(tax_vec);
	view.index_mode = ChooseAlignedIndexMode(filter_sel, count, scan_count);
	(void)composed_returnflag_sel;
	(void)composed_linestatus_sel;
	return true;
}

inline duckdb::idx_t AlignedPayloadIndex(const AlignedAggView &view, duckdb::idx_t row) {
	if (view.filter_sel && view.filter_sel->IsSet()) {
		return view.filter_sel->get_index(row);
	}
	return row;
}

inline duckdb::idx_t AlignedReturnflagIndex(const AlignedAggView &view, duckdb::idx_t row, duckdb::idx_t payload_index) {
	if (view.index_mode == AlignedIndexMode::Composed) {
		return view.composed_returnflag_sel->get_index(row);
	}
	return view.returnflag_dict_sel->get_index(payload_index);
}

inline duckdb::idx_t AlignedLinestatusIndex(const AlignedAggView &view, duckdb::idx_t row, duckdb::idx_t payload_index) {
	if (view.index_mode == AlignedIndexMode::Composed) {
		return view.composed_linestatus_sel->get_index(row);
	}
	return view.linestatus_dict_sel->get_index(payload_index);
}

bool BuildByteCacheFromAlignedView(duckdb::Vector &returnflag_vec, duckdb::Vector &linestatus_vec,
                                   const AlignedAggView &view, duckdb::idx_t count, DominantStringByteCache &cache,
                                   std::vector<uint8_t> &returnflag_seen, std::vector<uint8_t> &linestatus_seen) {
	const auto rf_dict_size = duckdb::DictionaryVector::DictionarySize(returnflag_vec);
	const auto ls_dict_size = duckdb::DictionaryVector::DictionarySize(linestatus_vec);
	if (!rf_dict_size.IsValid() || !ls_dict_size.IsValid()) {
		return false;
	}
	cache.returnflag_dictionary_size = rf_dict_size.GetIndex();
	cache.linestatus_dictionary_size = ls_dict_size.GetIndex();
	cache.returnflag_chars.resize(cache.returnflag_dictionary_size);
	cache.linestatus_chars.resize(cache.linestatus_dictionary_size);
	returnflag_seen.assign(cache.returnflag_dictionary_size, 0);
	linestatus_seen.assign(cache.linestatus_dictionary_size, 0);

	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto payload_index = AlignedPayloadIndex(view, row);
		const auto returnflag_index = AlignedReturnflagIndex(view, row, payload_index);
		if (returnflag_index >= cache.returnflag_dictionary_size) {
			return false;
		}
		if (!returnflag_seen[returnflag_index]) {
			if ((!view.returnflag_all_valid && !view.returnflag_validity->RowIsValidUnsafe(returnflag_index))) {
				return false;
			}
			const auto returnflag = view.returnflags[returnflag_index];
			if (returnflag.GetSize() != 1) {
				return false;
			}
			cache.returnflag_chars[returnflag_index] = static_cast<uint8_t>(returnflag.GetDataUnsafe()[0]);
			returnflag_seen[returnflag_index] = 1;
		}

		const auto linestatus_index = AlignedLinestatusIndex(view, row, payload_index);
		if (linestatus_index >= cache.linestatus_dictionary_size) {
			return false;
		}
		if (!linestatus_seen[linestatus_index]) {
			if ((!view.linestatus_all_valid && !view.linestatus_validity->RowIsValidUnsafe(linestatus_index))) {
				return false;
			}
			const auto linestatus = view.linestatuses[linestatus_index];
			if (linestatus.GetSize() != 1) {
				return false;
			}
			cache.linestatus_chars[linestatus_index] = static_cast<uint8_t>(linestatus.GetDataUnsafe()[0]);
			linestatus_seen[linestatus_index] = 1;
		}
	}
	return true;
}

template <bool SimplifyDerived>
inline void UpdateChunkLocalState(Q1ParallelChunkLocalState &state, int64_t quantity_raw, int64_t extendedprice_raw,
                                  int64_t discount_raw, int64_t tax_raw) {
	state.sum_qty += quantity_raw;
	state.sum_base_price += extendedprice_raw;
	state.sum_discount += discount_raw;
	state.count += 1;
	const auto one_minus_discount_raw = static_cast<int64_t>(100 - discount_raw);
	if constexpr (SimplifyDerived) {
		const auto disc_price_raw = extendedprice_raw * one_minus_discount_raw;
		state.sum_disc_price += disc_price_raw;
		state.sum_charge += disc_price_raw * static_cast<int64_t>(100 + tax_raw);
	} else {
		state.sum_disc_price += extendedprice_raw * one_minus_discount_raw;
		const auto one_plus_tax_raw = static_cast<int64_t>(100 + tax_raw);
		state.sum_charge += extendedprice_raw * one_minus_discount_raw * one_plus_tax_raw;
	}
}

inline void ConsumeAlignedRows(const AlignedAggView &view, duckdb::idx_t begin, duckdb::idx_t end,
                               duckdb::idx_t scan_count, Q1ParallelAlignedWorkerCache &cache) {
	const auto ls_dict_size = cache.linestatus_dictionary_size;

	for (duckdb::idx_t row = begin; row < end; row++) {
		const auto numeric_index = AlignedPayloadIndex(view, row);
		if (numeric_index >= scan_count) {
			continue;
		}
		const auto returnflag_index = AlignedReturnflagIndex(view, row, numeric_index);
		const auto linestatus_index = AlignedLinestatusIndex(view, row, numeric_index);
		const auto pair_index = returnflag_index * ls_dict_size + linestatus_index;
		if (pair_index >= cache.pair_to_slot.size()) {
			continue;
		}
		const auto slot = cache.pair_to_slot[pair_index];
		if (slot == kInvalidSlot || slot >= kMaxQ1Groups) {
			continue;
		}
		auto &local = cache.bank_states[row & (kMaxFixedSlotBanks - 1)][slot];
		UpdateChunkLocalState<false>(local, view.quantities[numeric_index], view.extendedprices[numeric_index],
		                            view.discounts[numeric_index], view.taxes[numeric_index]);
	}
}

inline void ConsumeAlignedChunk(const AlignedAggView &view, duckdb::idx_t count, duckdb::idx_t scan_count,
                                Q1ParallelAlignedWorkerCache &cache) {
	duckdb::idx_t row = 0;
	const auto limit = count - (count % 8);
	for (; row < limit; row += 8) {
		ConsumeAlignedRows(view, row, row + 8, scan_count, cache);
	}
	ConsumeAlignedRows(view, row, count, scan_count, cache);
}

void AggregateAlignedFixedSlot(const AlignedAggView &view, duckdb::idx_t count, duckdb::idx_t scan_count,
                               Q1ParallelAlignedWorkerCache &cache) {
	ConsumeAlignedChunk(view, count, scan_count, cache);
}

void MergeChunkLocalIntoGroup(Q1ParallelGroupState &state, const Q1ParallelChunkLocalState &local, int slot,
                              std::vector<uint16_t> &used_keys) {
	if (local.count == 0) {
		return;
	}
	if (!state.used) {
		state.used = true;
		used_keys.push_back(Q1SlotPackedKey(slot));
	}
	AddValue(state.sum_qty, local.sum_qty);
	AddValue(state.sum_base_price, local.sum_base_price);
	AddValue(state.sum_discount, local.sum_discount);
	AddValue(state.sum_disc_price, local.sum_disc_price);
	AddValue(state.sum_charge, local.sum_charge);
	state.count += static_cast<uint64_t>(local.count);
}

void MergeBanksIntoGroups(std::array<Q1ParallelGroupState, kMaxQ1Groups> &groups, std::vector<uint16_t> &used_keys,
                          Q1ParallelAlignedWorkerCache &cache) {
	for (duckdb::idx_t bank = 0; bank < kMaxFixedSlotBanks; bank++) {
		for (duckdb::idx_t slot = 0; slot < kMaxQ1Groups; slot++) {
			MergeChunkLocalIntoGroup(groups[slot], cache.bank_states[bank][slot], static_cast<int>(slot), used_keys);
			cache.bank_states[bank][slot] = {};
		}
	}
}

} // namespace

bool AccumulateQ1ParallelAlignedPayload(duckdb::Vector &quantity_vec, duckdb::Vector &extendedprice_vec,
                                        duckdb::Vector &discount_vec, duckdb::Vector &tax_vec,
                                        duckdb::Vector &returnflag_vec, duckdb::Vector &linestatus_vec,
                                        duckdb::SelectionVector &sel, duckdb::idx_t count, duckdb::idx_t scan_count,
                                        std::array<Q1ParallelGroupState, 6> &groups, std::vector<uint16_t> &used_keys,
                                        Q1ParallelAlignedWorkerCache &cache, duckdb::SelectionVector &composed_rf_sel,
                                        duckdb::SelectionVector &composed_ls_sel) {
	if (!sel.IsSet() || count < 100) {
		return false;
	}

	AlignedAggView aligned_view;
	if (!TryGetAlignedAggView(quantity_vec, extendedprice_vec, discount_vec, tax_vec, returnflag_vec, linestatus_vec, sel,
	                          count, scan_count, composed_rf_sel, composed_ls_sel, aligned_view)) {
		return false;
	}
	aligned_view.index_mode = AlignedIndexMode::Nested;

	if (!CanReuseAlignedSlotCache(aligned_view, cache, returnflag_vec, linestatus_vec)) {
		if (!RebuildAlignedSlotCache(returnflag_vec, linestatus_vec, aligned_view, count, cache)) {
			return false;
		}
	}
	ResetBankStates(cache);

	AggregateAlignedFixedSlot(aligned_view, count, scan_count, cache);

	MergeBanksIntoGroups(groups, used_keys, cache);
	return true;
}

} // namespace accelerator_codegen
