#include "accelerator_codegen/tpch_q1_fused.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kDecimalScale2 = 2;
constexpr uint8_t kDecimalScale4 = 4;
constexpr uint8_t kDecimalScale6 = 6;
constexpr uint8_t kDecimalWidth38 = 38;
constexpr duckdb::idx_t kQ1OutputBatchSize = 2;
constexpr duckdb::idx_t kShipdateScanIndex = 0;
constexpr duckdb::idx_t kQuantityScanIndex = 1;
constexpr duckdb::idx_t kExtendedpriceScanIndex = 2;
constexpr duckdb::idx_t kDiscountScanIndex = 3;
constexpr duckdb::idx_t kTaxScanIndex = 4;
constexpr duckdb::idx_t kReturnflagScanIndex = 5;
constexpr duckdb::idx_t kLinestatusScanIndex = 6;
constexpr duckdb::idx_t kProjectedColumnCount = 7;
constexpr duckdb::idx_t kFixedSlotComboLimit = 256;
constexpr duckdb::idx_t kFixedSlotStateLimit = 16;
constexpr duckdb::idx_t kMaxFixedSlotBanks = 8;

#if defined(__SIZEOF_INT128__)
using wide_hugeint_t = __int128_t;
using q1_acc_t = wide_hugeint_t;

duckdb::hugeint_t FromWideHugeint(wide_hugeint_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}

inline void AddValue(q1_acc_t &accumulator, int64_t value) {
	accumulator += static_cast<q1_acc_t>(value);
}
#else
using wide_hugeint_t = duckdb::hugeint_t;
using q1_acc_t = wide_hugeint_t;

duckdb::hugeint_t FromWideHugeint(wide_hugeint_t value) {
	return value;
}

inline void AddValue(q1_acc_t &accumulator, int64_t input) {
	auto addend = duckdb::Hugeint::Convert(input);
	if (!duckdb::Hugeint::TryAddInPlace(accumulator, addend)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q1 fused aggregate");
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

inline uint16_t PackGroupKeyUnchecked(duckdb::string_t returnflag, duckdb::string_t linestatus) {
	auto lhs = static_cast<uint8_t>(returnflag.GetDataUnsafe()[0]);
	auto rhs = static_cast<uint8_t>(linestatus.GetDataUnsafe()[0]);
	return static_cast<uint16_t>((static_cast<uint16_t>(lhs) << 8) | rhs);
}

inline uint16_t PackGroupKeyUncheckedBytes(uint8_t returnflag_char, uint8_t linestatus_char) {
	return static_cast<uint16_t>((static_cast<uint16_t>(returnflag_char) << 8) | linestatus_char);
}

std::string UnpackLeft(uint16_t key) {
	return std::string(1, static_cast<char>(key >> 8));
}

std::string UnpackRight(uint16_t key) {
	return std::string(1, static_cast<char>(key & 0xFF));
}

#ifdef ACCEL_EXPERIMENTAL_Q1_OPTS
bool ReadBoolEnv(const char *name, bool fallback) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		return fallback;
	}
	return value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y';
}

int ReadPositiveEnvInt(const char *name, int fallback) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		return fallback;
	}
	const auto parsed = std::atoi(value);
	return parsed > 0 ? parsed : fallback;
}
#endif

struct GroupState {
	q1_acc_t sum_qty = 0;
	q1_acc_t sum_base_price = 0;
	q1_acc_t sum_disc_price = 0;
	q1_acc_t sum_charge = 0;
	q1_acc_t sum_discount = 0;
	int64_t pending_sum_qty = 0;
	int64_t pending_sum_base_price = 0;
	int64_t pending_sum_disc_price = 0;
	int64_t pending_sum_charge = 0;
	int64_t pending_sum_discount = 0;
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

struct ChunkLocalAggregateState {
	int64_t sum_qty = 0;
	int64_t sum_base_price = 0;
	int64_t sum_disc_price = 0;
	int64_t sum_charge = 0;
	int64_t sum_discount = 0;
	int64_t count = 0;
};

struct FusedHotLoopConfig {
	uint8_t fixed_slot_banks = 1;
	uint8_t fixed_slot_unroll = 1;
	bool use_group_int64_partials = false;
	bool simplify_exact_derived = false;
};

FusedHotLoopConfig LoadFusedHotLoopConfig() {
	FusedHotLoopConfig config;
#ifdef ACCEL_EXPERIMENTAL_Q1_OPTS
	const auto requested_banks = ReadPositiveEnvInt("ACCEL_Q1_FIXED_SLOT_BANKS", 1);
	switch (requested_banks) {
	case 1:
	case 2:
	case 4:
	case 8:
		config.fixed_slot_banks = static_cast<uint8_t>(requested_banks);
		break;
	default:
		config.fixed_slot_banks = 1;
		break;
	}
	const auto requested_unroll = ReadPositiveEnvInt("ACCEL_Q1_FIXED_SLOT_UNROLL", 1);
	config.fixed_slot_unroll = requested_unroll >= 8 ? 8 : (requested_unroll >= 4 ? 4 : 1);
	config.use_group_int64_partials = ReadBoolEnv("ACCEL_Q1_GROUP_INT64_PARTIALS", false);
	config.simplify_exact_derived = ReadBoolEnv("ACCEL_Q1_SIMPLIFY_DERIVED", false);
#endif
	return config;
}

const FusedHotLoopConfig &GetFusedHotLoopConfig() {
	static const auto config = LoadFusedHotLoopConfig();
	return config;
}

template <bool SimplifyDerived>
inline void UpdateChunkLocalState(ChunkLocalAggregateState &state, int64_t quantity_raw, int64_t extendedprice_raw,
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

inline void FlushGroupPendingPartials(GroupState &state) {
	if (state.pending_sum_qty == 0 && state.pending_sum_base_price == 0 && state.pending_sum_disc_price == 0 &&
	    state.pending_sum_charge == 0 && state.pending_sum_discount == 0) {
		return;
	}
	AddValue(state.sum_qty, state.pending_sum_qty);
	AddValue(state.sum_base_price, state.pending_sum_base_price);
	AddValue(state.sum_discount, state.pending_sum_discount);
	AddValue(state.sum_disc_price, state.pending_sum_disc_price);
	AddValue(state.sum_charge, state.pending_sum_charge);
	state.pending_sum_qty = 0;
	state.pending_sum_base_price = 0;
	state.pending_sum_disc_price = 0;
	state.pending_sum_charge = 0;
	state.pending_sum_discount = 0;
}

void MergeChunkLocalIntoGroup(GroupState &state, const ChunkLocalAggregateState &local, bool use_group_int64_partials) {
	if (!use_group_int64_partials) {
		AddValue(state.sum_qty, local.sum_qty);
		AddValue(state.sum_base_price, local.sum_base_price);
		AddValue(state.sum_discount, local.sum_discount);
		AddValue(state.sum_disc_price, local.sum_disc_price);
		AddValue(state.sum_charge, local.sum_charge);
		state.count += static_cast<uint64_t>(local.count);
		return;
	}
	auto append_partial = [&](int64_t &pending, int64_t delta) {
		if (__builtin_add_overflow(pending, delta, &pending)) {
			FlushGroupPendingPartials(state);
			if (__builtin_add_overflow(pending, delta, &pending)) {
				throw duckdb::OutOfRangeException("Overflow while accumulating Q1 fused int64 partials");
			}
		}
	};
	append_partial(state.pending_sum_qty, local.sum_qty);
	append_partial(state.pending_sum_base_price, local.sum_base_price);
	append_partial(state.pending_sum_discount, local.sum_discount);
	append_partial(state.pending_sum_disc_price, local.sum_disc_price);
	append_partial(state.pending_sum_charge, local.sum_charge);
	state.count += static_cast<uint64_t>(local.count);
}

inline void UpdateGroupDirect(GroupState &state, std::vector<uint16_t> &used_keys, uint16_t group_key,
                              int64_t quantity_raw, int64_t extendedprice_raw, int64_t discount_raw, int64_t tax_raw,
                              bool use_group_int64_partials, bool simplify) {
	if (!state.used) {
		state.used = true;
		used_keys.push_back(group_key);
	}
	ChunkLocalAggregateState local;
	if (simplify) {
		UpdateChunkLocalState<true>(local, quantity_raw, extendedprice_raw, discount_raw, tax_raw);
	} else {
		UpdateChunkLocalState<false>(local, quantity_raw, extendedprice_raw, discount_raw, tax_raw);
	}
	MergeChunkLocalIntoGroup(state, local, use_group_int64_partials);
}

template <class T>
const T *GetFlatChildData(duckdb::Vector &vector) {
	auto &child = duckdb::DictionaryVector::Child(vector);
	return duckdb::FlatVector::GetData<T>(child);
}

bool IsDictionaryOverFlatAllValid(duckdb::Vector &vector) {
	if (vector.GetVectorType() != duckdb::VectorType::DICTIONARY_VECTOR) {
		return false;
	}
	auto &child = duckdb::DictionaryVector::Child(vector);
	return child.GetVectorType() == duckdb::VectorType::FLAT_VECTOR && duckdb::FlatVector::Validity(child).AllValid();
}

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR && duckdb::FlatVector::Validity(vector).AllValid();
}

duckdb::idx_t DictionarySizeOrReferencedSpan(const duckdb::Vector &vector, const duckdb::SelectionVector &sel,
                                             duckdb::idx_t count) {
	const auto dictionary_size = duckdb::DictionaryVector::DictionarySize(vector);
	if (dictionary_size.IsValid()) {
		return dictionary_size.GetIndex();
	}
	duckdb::idx_t max_index = 0;
	for (duckdb::idx_t row = 0; row < count; row++) {
		max_index = std::max(max_index, static_cast<duckdb::idx_t>(sel.get_index(row)));
	}
	return count == 0 ? 0 : max_index + 1;
}

struct FusedDominantView {
	const duckdb::SelectionVector *numeric_sel = nullptr;
	const duckdb::SelectionVector *returnflag_sel = nullptr;
	const duckdb::SelectionVector *linestatus_sel = nullptr;
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

bool SelectionValuesMatch(const duckdb::SelectionVector &lhs, const duckdb::SelectionVector &rhs, duckdb::idx_t count) {
	for (duckdb::idx_t row = 0; row < count; row++) {
		if (lhs.get_index(row) != rhs.get_index(row)) {
			return false;
		}
	}
	return true;
}

void RecordLayout(Q1FusedLayoutCounters &layouts, duckdb::Vector &quantity_vec, duckdb::Vector &extendedprice_vec,
                  duckdb::Vector &discount_vec, duckdb::Vector &tax_vec, duckdb::Vector &returnflag_vec,
                  duckdb::Vector &linestatus_vec, duckdb::idx_t count) {
	layouts.vectors_inspected += 1;

	auto bump_type = [](duckdb::Vector &v, duckdb::idx_t &dict, duckdb::idx_t &flat, duckdb::idx_t &other) {
		switch (v.GetVectorType()) {
		case duckdb::VectorType::DICTIONARY_VECTOR:
			dict += 1;
			break;
		case duckdb::VectorType::FLAT_VECTOR:
			flat += 1;
			break;
		default:
			other += 1;
			break;
		}
	};
	bump_type(quantity_vec, layouts.qty_dictionary, layouts.qty_flat, layouts.qty_other);
	duckdb::idx_t ep_other = 0, disc_other = 0, tax_other = 0;
	bump_type(extendedprice_vec, layouts.ep_dictionary, layouts.ep_flat, ep_other);
	bump_type(discount_vec, layouts.disc_dictionary, layouts.disc_flat, disc_other);
	bump_type(tax_vec, layouts.tax_dictionary, layouts.tax_flat, tax_other);
	bump_type(returnflag_vec, layouts.rf_dictionary, layouts.rf_flat, layouts.rf_other);
	bump_type(linestatus_vec, layouts.ls_dictionary, layouts.ls_flat, layouts.ls_other);
	(void)ep_other;
	(void)disc_other;
	(void)tax_other;

	duckdb::Vector *numeric_vectors[] = {&quantity_vec, &extendedprice_vec, &discount_vec, &tax_vec};
	bool all_dict_flat = true;
	const duckdb::SelectionVector *first_sel = nullptr;
	bool shared_ptr = true;
	bool shared_values = true;
	for (auto *vector : numeric_vectors) {
		if (!IsDictionaryOverFlatAllValid(*vector)) {
			all_dict_flat = false;
			shared_ptr = false;
			shared_values = false;
			break;
		}
		auto &sel = duckdb::DictionaryVector::SelVector(*vector);
		if (!first_sel) {
			first_sel = &sel;
			layouts.qty_child_all_valid += 1;
		} else if (first_sel->data() != sel.data()) {
			shared_ptr = false;
			if (!SelectionValuesMatch(*first_sel, sel, count)) {
				shared_values = false;
			}
		}
	}
	if (all_dict_flat) {
		if (shared_ptr) {
			layouts.numeric_shared_sel_ptr += 1;
			layouts.numeric_shared_sel_values += 1;
		} else if (shared_values) {
			layouts.numeric_shared_sel_values += 1;
		} else {
			layouts.numeric_divergent_sel += 1;
		}
	}

	if (returnflag_vec.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR) {
		auto &child = duckdb::DictionaryVector::Child(returnflag_vec);
		if (child.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
		    duckdb::FlatVector::Validity(child).AllValid()) {
			layouts.rf_child_all_valid += 1;
		}
	}
	if (linestatus_vec.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR) {
		auto &child = duckdb::DictionaryVector::Child(linestatus_vec);
		if (child.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
		    duckdb::FlatVector::Validity(child).AllValid()) {
			layouts.ls_child_all_valid += 1;
		}
	}

	const bool flat_ok = IsFlatAllValid(quantity_vec) && IsFlatAllValid(extendedprice_vec) &&
	                     IsFlatAllValid(discount_vec) && IsFlatAllValid(tax_vec) && IsFlatAllValid(returnflag_vec) &&
	                     IsFlatAllValid(linestatus_vec);
	if (flat_ok) {
		layouts.flat_eligible += 1;
	}

	FusedDominantView probe;
	if (all_dict_flat && shared_values &&
	    returnflag_vec.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR &&
	    linestatus_vec.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR) {
		auto &rf_child = duckdb::DictionaryVector::Child(returnflag_vec);
		auto &ls_child = duckdb::DictionaryVector::Child(linestatus_vec);
		if (rf_child.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
		    ls_child.GetVectorType() == duckdb::VectorType::FLAT_VECTOR) {
			layouts.dominant_eligible += 1;
			(void)probe;
		}
	}
}

bool TryGetFusedDominantView(duckdb::Vector &quantity_vec, duckdb::Vector &extendedprice_vec,
                             duckdb::Vector &discount_vec, duckdb::Vector &tax_vec, duckdb::Vector &returnflag_vec,
                             duckdb::Vector &linestatus_vec, duckdb::idx_t count, FusedDominantView &view) {
	duckdb::Vector *numeric_vectors[] = {&quantity_vec, &extendedprice_vec, &discount_vec, &tax_vec};
	const duckdb::SelectionVector *numeric_sel = nullptr;
	for (auto *vector : numeric_vectors) {
		if (!IsDictionaryOverFlatAllValid(*vector)) {
			return false;
		}
		auto &sel = duckdb::DictionaryVector::SelVector(*vector);
		if (!numeric_sel) {
			numeric_sel = &sel;
		} else if (numeric_sel->data() != sel.data() && !SelectionValuesMatch(*numeric_sel, sel, count)) {
			return false;
		}
	}

	if (returnflag_vec.GetVectorType() != duckdb::VectorType::DICTIONARY_VECTOR ||
	    linestatus_vec.GetVectorType() != duckdb::VectorType::DICTIONARY_VECTOR) {
		return false;
	}
	auto &returnflag_child = duckdb::DictionaryVector::Child(returnflag_vec);
	auto &linestatus_child = duckdb::DictionaryVector::Child(linestatus_vec);
	if (returnflag_child.GetVectorType() != duckdb::VectorType::FLAT_VECTOR ||
	    linestatus_child.GetVectorType() != duckdb::VectorType::FLAT_VECTOR) {
		return false;
	}

	view.numeric_sel = numeric_sel;
	view.returnflag_sel = &duckdb::DictionaryVector::SelVector(returnflag_vec);
	view.linestatus_sel = &duckdb::DictionaryVector::SelVector(linestatus_vec);
	view.quantities = GetFlatChildData<int64_t>(quantity_vec);
	view.extendedprices = GetFlatChildData<int64_t>(extendedprice_vec);
	view.discounts = GetFlatChildData<int64_t>(discount_vec);
	view.taxes = GetFlatChildData<int64_t>(tax_vec);
	view.returnflags = GetFlatChildData<duckdb::string_t>(returnflag_vec);
	view.linestatuses = GetFlatChildData<duckdb::string_t>(linestatus_vec);
	view.returnflag_validity = &duckdb::FlatVector::Validity(returnflag_child);
	view.linestatus_validity = &duckdb::FlatVector::Validity(linestatus_child);
	view.returnflag_all_valid = view.returnflag_validity->AllValid();
	view.linestatus_all_valid = view.linestatus_validity->AllValid();
	return true;
}

//! Compose dictionary selection through the post-filter sel into `composed` (size `count`).
void ComposeDictionarySel(const duckdb::SelectionVector &filter_sel, const duckdb::SelectionVector &dict_sel,
                          duckdb::idx_t count, duckdb::SelectionVector &composed) {
	for (duckdb::idx_t row = 0; row < count; row++) {
		composed.set_index(row, dict_sel.get_index(filter_sel.get_index(row)));
	}
}

enum class AlignedIndexMode : uint8_t {
	//! filter_sel is identity: row i maps to payload position i.
	IdentityFilter = 0,
	//! numeric_index = filter[row]; rf/ls = dict[numeric_index] (no composed buffers).
	Nested = 1,
	//! Materialize composed rf/ls sels of size `count` (legacy / A/B).
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

bool FilterSelIsIdentity(const duckdb::SelectionVector &filter_sel) {
	return !filter_sel.IsSet();
}

bool FilterSelIsDensePrefix(const duckdb::SelectionVector &filter_sel, duckdb::idx_t count,
                            duckdb::idx_t scan_count) {
	// Dense 0..count-1 is equivalent to identity only when the whole scanned vector qualifies.
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
                                        duckdb::idx_t scan_count, bool force_compose) {
	if (force_compose) {
		return AlignedIndexMode::Composed;
	}
	if (FilterSelIsIdentity(filter_sel) || FilterSelIsDensePrefix(filter_sel, count, scan_count)) {
		return AlignedIndexMode::IdentityFilter;
	}
	return AlignedIndexMode::Nested;
}

//! Build a fixed-slot view over Scan-without-Slice payload blocks + post-filter selection.
bool TryGetAlignedAggView(duckdb::Vector &quantity_vec, duckdb::Vector &extendedprice_vec,
                          duckdb::Vector &discount_vec, duckdb::Vector &tax_vec, duckdb::Vector &returnflag_vec,
                          duckdb::Vector &linestatus_vec, duckdb::SelectionVector &filter_sel, duckdb::idx_t count,
                          duckdb::idx_t scan_count, bool force_compose,
                          duckdb::SelectionVector &composed_returnflag_sel,
                          duckdb::SelectionVector &composed_linestatus_sel, AlignedAggView &view,
                          duckdb::idx_t &compose_ns) {
	compose_ns = 0;
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
	view.index_mode = ChooseAlignedIndexMode(filter_sel, count, scan_count, force_compose);

	if (view.index_mode == AlignedIndexMode::Composed) {
		const auto compose_start = std::chrono::steady_clock::now();
		ComposeDictionarySel(filter_sel, *view.returnflag_dict_sel, count, composed_returnflag_sel);
		ComposeDictionarySel(filter_sel, *view.linestatus_dict_sel, count, composed_linestatus_sel);
		compose_ns = static_cast<duckdb::idx_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - compose_start)
		        .count());
		view.composed_returnflag_sel = &composed_returnflag_sel;
		view.composed_linestatus_sel = &composed_linestatus_sel;
	}
	return true;
}

struct DominantStringByteCache {
	duckdb::idx_t returnflag_dictionary_size = 0;
	duckdb::idx_t linestatus_dictionary_size = 0;
	std::vector<uint8_t> returnflag_chars;
	std::vector<uint8_t> linestatus_chars;
};

struct DominantPackedKeyCache {
	duckdb::idx_t returnflag_dictionary_size = 0;
	duckdb::idx_t linestatus_dictionary_size = 0;
	std::vector<uint16_t> packed_keys;
};

struct ReusableFixedSlotCache {
	bool valid = false;
	bool use_fixed_slots = false;
	const duckdb::string_t *returnflag_child = nullptr;
	const duckdb::string_t *linestatus_child = nullptr;
	const duckdb::ValidityMask *returnflag_validity = nullptr;
	const duckdb::ValidityMask *linestatus_validity = nullptr;
	bool returnflag_all_valid = false;
	bool linestatus_all_valid = false;
	duckdb::idx_t returnflag_dictionary_size = 0;
	duckdb::idx_t linestatus_dictionary_size = 0;
	DominantStringByteCache byte_cache;
	DominantPackedKeyCache packed_key_cache;
	std::vector<uint8_t> returnflag_seen;
	std::vector<uint8_t> linestatus_seen;
	std::array<uint8_t, kFixedSlotComboLimit> fixed_pair_to_slot {};
	std::array<uint16_t, kFixedSlotStateLimit> fixed_slot_keys {};
	std::array<GroupState *, kFixedSlotStateLimit> fixed_slot_targets {};
	std::array<std::array<ChunkLocalAggregateState, kFixedSlotStateLimit>, kMaxFixedSlotBanks> fixed_slot_bank_states {};
	duckdb::idx_t fixed_slot_count = 0;
};

bool BuildByteCacheFromView(duckdb::Vector &returnflag_vec, duckdb::Vector &linestatus_vec, const FusedDominantView &view,
                            duckdb::idx_t count, DominantStringByteCache &cache, std::vector<uint8_t> &returnflag_seen,
                            std::vector<uint8_t> &linestatus_seen) {
	cache.returnflag_dictionary_size = DictionarySizeOrReferencedSpan(returnflag_vec, *view.returnflag_sel, count);
	cache.linestatus_dictionary_size = DictionarySizeOrReferencedSpan(linestatus_vec, *view.linestatus_sel, count);
	cache.returnflag_chars.resize(cache.returnflag_dictionary_size);
	cache.linestatus_chars.resize(cache.linestatus_dictionary_size);
	returnflag_seen.assign(cache.returnflag_dictionary_size, 0);
	linestatus_seen.assign(cache.linestatus_dictionary_size, 0);

	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto returnflag_index = view.returnflag_sel->get_index(row);
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

		const auto linestatus_index = view.linestatus_sel->get_index(row);
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

inline duckdb::idx_t AlignedPayloadIndex(const AlignedAggView &view, duckdb::idx_t row) {
	if (view.index_mode == AlignedIndexMode::IdentityFilter) {
		return row;
	}
	return view.filter_sel->get_index(row);
}

inline duckdb::idx_t AlignedReturnflagIndex(const AlignedAggView &view, duckdb::idx_t row,
                                            duckdb::idx_t payload_index) {
	if (view.index_mode == AlignedIndexMode::Composed) {
		return view.composed_returnflag_sel->get_index(row);
	}
	return view.returnflag_dict_sel->get_index(payload_index);
}

inline duckdb::idx_t AlignedLinestatusIndex(const AlignedAggView &view, duckdb::idx_t row,
                                            duckdb::idx_t payload_index) {
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

void BuildPackedKeyCache(const DominantStringByteCache &byte_cache, DominantPackedKeyCache &packed_key_cache) {
	packed_key_cache.returnflag_dictionary_size = byte_cache.returnflag_dictionary_size;
	packed_key_cache.linestatus_dictionary_size = byte_cache.linestatus_dictionary_size;
	packed_key_cache.packed_keys.assign(
	    packed_key_cache.returnflag_dictionary_size * packed_key_cache.linestatus_dictionary_size, 0);
	for (duckdb::idx_t returnflag_index = 0; returnflag_index < packed_key_cache.returnflag_dictionary_size;
	     returnflag_index++) {
		const auto returnflag_char = byte_cache.returnflag_chars[returnflag_index];
		const auto base = returnflag_index * packed_key_cache.linestatus_dictionary_size;
		for (duckdb::idx_t linestatus_index = 0; linestatus_index < packed_key_cache.linestatus_dictionary_size;
		     linestatus_index++) {
			packed_key_cache.packed_keys[base + linestatus_index] =
			    PackGroupKeyUncheckedBytes(returnflag_char, byte_cache.linestatus_chars[linestatus_index]);
		}
	}
}

bool RebuildFixedSlotCache(duckdb::Vector &returnflag_vec, duckdb::Vector &linestatus_vec, const FusedDominantView &view,
                           duckdb::idx_t count, std::array<GroupState, 1u << 16> &groups, ReusableFixedSlotCache &cache) {
	if (!BuildByteCacheFromView(returnflag_vec, linestatus_vec, view, count, cache.byte_cache, cache.returnflag_seen,
	                            cache.linestatus_seen)) {
		cache.valid = false;
		return false;
	}
	BuildPackedKeyCache(cache.byte_cache, cache.packed_key_cache);
	cache.returnflag_dictionary_size = cache.byte_cache.returnflag_dictionary_size;
	cache.linestatus_dictionary_size = cache.byte_cache.linestatus_dictionary_size;
	cache.returnflag_child = view.returnflags;
	cache.linestatus_child = view.linestatuses;
	cache.returnflag_validity = view.returnflag_validity;
	cache.linestatus_validity = view.linestatus_validity;
	cache.returnflag_all_valid = view.returnflag_all_valid;
	cache.linestatus_all_valid = view.linestatus_all_valid;

	const auto combo_count = cache.packed_key_cache.packed_keys.size();
	if (combo_count > kFixedSlotComboLimit) {
		cache.valid = false;
		return false;
	}

	cache.fixed_slot_count = 0;
	cache.use_fixed_slots = true;
	cache.fixed_pair_to_slot.fill(static_cast<uint8_t>(0xFF));

	for (duckdb::idx_t pair_index = 0; pair_index < combo_count; pair_index++) {
		const auto key = cache.packed_key_cache.packed_keys[pair_index];
		uint8_t fixed_slot = static_cast<uint8_t>(0xFF);
		for (duckdb::idx_t idx = 0; idx < cache.fixed_slot_count; idx++) {
			if (cache.fixed_slot_keys[idx] == key) {
				fixed_slot = static_cast<uint8_t>(idx);
				break;
			}
		}
		if (fixed_slot == static_cast<uint8_t>(0xFF)) {
			if (cache.fixed_slot_count >= kFixedSlotStateLimit) {
				cache.use_fixed_slots = false;
				cache.valid = false;
				return false;
			}
			fixed_slot = static_cast<uint8_t>(cache.fixed_slot_count);
			cache.fixed_slot_keys[cache.fixed_slot_count] = key;
			cache.fixed_slot_targets[cache.fixed_slot_count] = &groups[key];
			for (duckdb::idx_t bank = 0; bank < kMaxFixedSlotBanks; bank++) {
				cache.fixed_slot_bank_states[bank][cache.fixed_slot_count] = {};
			}
			cache.fixed_slot_count += 1;
		}
		cache.fixed_pair_to_slot[pair_index] = fixed_slot;
	}

	cache.valid = true;
	return true;
}

bool CanReuseFixedSlotCache(const FusedDominantView &view, const ReusableFixedSlotCache &cache,
                            duckdb::Vector &returnflag_vec, duckdb::Vector &linestatus_vec, duckdb::idx_t count) {
	if (!cache.valid || !cache.use_fixed_slots) {
		return false;
	}
	const auto rf_size = DictionarySizeOrReferencedSpan(returnflag_vec, *view.returnflag_sel, count);
	const auto ls_size = DictionarySizeOrReferencedSpan(linestatus_vec, *view.linestatus_sel, count);
	return cache.returnflag_child == view.returnflags && cache.linestatus_child == view.linestatuses &&
	       cache.returnflag_validity == view.returnflag_validity && cache.linestatus_validity == view.linestatus_validity &&
	       cache.returnflag_all_valid == view.returnflag_all_valid &&
	       cache.linestatus_all_valid == view.linestatus_all_valid && cache.returnflag_dictionary_size == rf_size &&
	       cache.linestatus_dictionary_size == ls_size;
}

bool RebuildAlignedFixedSlotCache(duckdb::Vector &returnflag_vec, duckdb::Vector &linestatus_vec,
                                  const AlignedAggView &view, duckdb::idx_t count,
                                  std::array<GroupState, 1u << 16> &groups, ReusableFixedSlotCache &cache) {
	if (!BuildByteCacheFromAlignedView(returnflag_vec, linestatus_vec, view, count, cache.byte_cache,
	                                   cache.returnflag_seen, cache.linestatus_seen)) {
		cache.valid = false;
		return false;
	}
	BuildPackedKeyCache(cache.byte_cache, cache.packed_key_cache);
	cache.returnflag_dictionary_size = cache.byte_cache.returnflag_dictionary_size;
	cache.linestatus_dictionary_size = cache.byte_cache.linestatus_dictionary_size;
	cache.returnflag_child = view.returnflags;
	cache.linestatus_child = view.linestatuses;
	cache.returnflag_validity = view.returnflag_validity;
	cache.linestatus_validity = view.linestatus_validity;
	cache.returnflag_all_valid = view.returnflag_all_valid;
	cache.linestatus_all_valid = view.linestatus_all_valid;

	const auto combo_count = cache.packed_key_cache.packed_keys.size();
	if (combo_count > kFixedSlotComboLimit) {
		cache.valid = false;
		return false;
	}

	cache.fixed_slot_count = 0;
	cache.use_fixed_slots = true;
	cache.fixed_pair_to_slot.fill(static_cast<uint8_t>(0xFF));

	for (duckdb::idx_t pair_index = 0; pair_index < combo_count; pair_index++) {
		const auto key = cache.packed_key_cache.packed_keys[pair_index];
		uint8_t fixed_slot = static_cast<uint8_t>(0xFF);
		for (duckdb::idx_t idx = 0; idx < cache.fixed_slot_count; idx++) {
			if (cache.fixed_slot_keys[idx] == key) {
				fixed_slot = static_cast<uint8_t>(idx);
				break;
			}
		}
		if (fixed_slot == static_cast<uint8_t>(0xFF)) {
			if (cache.fixed_slot_count >= kFixedSlotStateLimit) {
				cache.use_fixed_slots = false;
				cache.valid = false;
				return false;
			}
			fixed_slot = static_cast<uint8_t>(cache.fixed_slot_count);
			cache.fixed_slot_keys[cache.fixed_slot_count] = key;
			cache.fixed_slot_targets[cache.fixed_slot_count] = &groups[key];
			for (duckdb::idx_t bank = 0; bank < kMaxFixedSlotBanks; bank++) {
				cache.fixed_slot_bank_states[bank][cache.fixed_slot_count] = {};
			}
			cache.fixed_slot_count += 1;
		}
		cache.fixed_pair_to_slot[pair_index] = fixed_slot;
	}

	cache.valid = true;
	return true;
}

bool CanReuseAlignedFixedSlotCache(const AlignedAggView &view, const ReusableFixedSlotCache &cache,
                                   duckdb::Vector &returnflag_vec, duckdb::Vector &linestatus_vec) {
	if (!cache.valid || !cache.use_fixed_slots) {
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

void ResetFixedSlotBankStates(ReusableFixedSlotCache &cache) {
	for (duckdb::idx_t bank = 0; bank < kMaxFixedSlotBanks; bank++) {
		for (duckdb::idx_t idx = 0; idx < cache.fixed_slot_count; idx++) {
			cache.fixed_slot_bank_states[bank][idx] = {};
		}
	}
}

template <duckdb::idx_t Banks, bool SimplifyDerived>
inline void ConsumeFixedSlotRows(const FusedDominantView &view, duckdb::idx_t begin, duckdb::idx_t end,
                                 ReusableFixedSlotCache &cache) {
	for (duckdb::idx_t row = begin; row < end; row++) {
		const auto numeric_index = view.numeric_sel->get_index(row);
		const auto returnflag_index = view.returnflag_sel->get_index(row);
		const auto linestatus_index = view.linestatus_sel->get_index(row);
		const auto pair_index = returnflag_index * cache.linestatus_dictionary_size + linestatus_index;
		const auto slot = cache.fixed_pair_to_slot[pair_index];
		auto &local = cache.fixed_slot_bank_states[row & (Banks - 1)][slot];
		UpdateChunkLocalState<SimplifyDerived>(local, view.quantities[numeric_index], view.extendedprices[numeric_index],
		                                       view.discounts[numeric_index], view.taxes[numeric_index]);
	}
}

template <duckdb::idx_t Banks, duckdb::idx_t Unroll, bool SimplifyDerived>
inline void ConsumeFixedSlotChunk(const FusedDominantView &view, duckdb::idx_t count, ReusableFixedSlotCache &cache) {
	duckdb::idx_t row = 0;
	if constexpr (Unroll > 1) {
		const auto limit = count - (count % Unroll);
		for (; row < limit; row += Unroll) {
			ConsumeFixedSlotRows<Banks, SimplifyDerived>(view, row, row + Unroll, cache);
		}
	}
	ConsumeFixedSlotRows<Banks, SimplifyDerived>(view, row, count, cache);
}

void MergeFixedSlotBanks(std::vector<uint16_t> &used_keys, ReusableFixedSlotCache &cache, bool use_group_int64_partials,
                         duckdb::idx_t bank_count) {
	for (duckdb::idx_t slot = 0; slot < cache.fixed_slot_count; slot++) {
		ChunkLocalAggregateState merged_local;
		for (duckdb::idx_t bank = 0; bank < bank_count; bank++) {
			const auto &local = cache.fixed_slot_bank_states[bank][slot];
			merged_local.sum_qty += local.sum_qty;
			merged_local.sum_base_price += local.sum_base_price;
			merged_local.sum_disc_price += local.sum_disc_price;
			merged_local.sum_charge += local.sum_charge;
			merged_local.sum_discount += local.sum_discount;
			merged_local.count += local.count;
		}
		if (merged_local.count == 0) {
			continue;
		}
		auto &state = *cache.fixed_slot_targets[slot];
		if (!state.used) {
			state.used = true;
			used_keys.push_back(cache.fixed_slot_keys[slot]);
		}
		MergeChunkLocalIntoGroup(state, merged_local, use_group_int64_partials);
	}
}

void AggregateFixedSlot(const FusedDominantView &view, duckdb::idx_t count, ReusableFixedSlotCache &cache,
                        std::vector<uint16_t> &used_keys, const FusedHotLoopConfig &config) {
	const auto banks = static_cast<duckdb::idx_t>(config.fixed_slot_banks);
	const auto unroll = config.fixed_slot_unroll;
	const auto simplify = config.simplify_exact_derived;
	if (banks == 8 && unroll == 8) {
		if (simplify) {
			ConsumeFixedSlotChunk<8, 8, true>(view, count, cache);
		} else {
			ConsumeFixedSlotChunk<8, 8, false>(view, count, cache);
		}
	} else if (banks == 4 && unroll >= 4) {
		if (simplify) {
			ConsumeFixedSlotChunk<4, 4, true>(view, count, cache);
		} else {
			ConsumeFixedSlotChunk<4, 4, false>(view, count, cache);
		}
	} else if (banks == 2) {
		if (simplify) {
			ConsumeFixedSlotChunk<2, 1, true>(view, count, cache);
		} else {
			ConsumeFixedSlotChunk<2, 1, false>(view, count, cache);
		}
	} else if (simplify) {
		ConsumeFixedSlotChunk<1, 1, true>(view, count, cache);
	} else {
		ConsumeFixedSlotChunk<1, 1, false>(view, count, cache);
	}
	MergeFixedSlotBanks(used_keys, cache, config.use_group_int64_partials, banks);
}

template <AlignedIndexMode Mode, duckdb::idx_t Banks, bool SimplifyDerived>
inline void ConsumeAlignedFixedSlotRows(const AlignedAggView &view, duckdb::idx_t begin, duckdb::idx_t end,
                                        ReusableFixedSlotCache &cache) {
	const auto *filter_data = view.filter_sel ? view.filter_sel->data() : nullptr;
	const auto *rf_dict = view.returnflag_dict_sel->data();
	const auto *ls_dict = view.linestatus_dict_sel->data();
	const auto *rf_composed =
	    view.composed_returnflag_sel ? view.composed_returnflag_sel->data() : nullptr;
	const auto *ls_composed =
	    view.composed_linestatus_sel ? view.composed_linestatus_sel->data() : nullptr;
	const auto ls_dict_size = cache.linestatus_dictionary_size;

	for (duckdb::idx_t row = begin; row < end; row++) {
		duckdb::idx_t numeric_index;
		duckdb::idx_t returnflag_index;
		duckdb::idx_t linestatus_index;
		if constexpr (Mode == AlignedIndexMode::IdentityFilter) {
			numeric_index = row;
			returnflag_index = rf_dict[row];
			linestatus_index = ls_dict[row];
		} else if constexpr (Mode == AlignedIndexMode::Nested) {
			numeric_index = filter_data ? filter_data[row] : row;
			returnflag_index = rf_dict[numeric_index];
			linestatus_index = ls_dict[numeric_index];
		} else {
			numeric_index = filter_data ? filter_data[row] : row;
			returnflag_index = rf_composed[row];
			linestatus_index = ls_composed[row];
		}
		const auto pair_index = returnflag_index * ls_dict_size + linestatus_index;
		const auto slot = cache.fixed_pair_to_slot[pair_index];
		auto &local = cache.fixed_slot_bank_states[row & (Banks - 1)][slot];
		UpdateChunkLocalState<SimplifyDerived>(local, view.quantities[numeric_index], view.extendedprices[numeric_index],
		                                       view.discounts[numeric_index], view.taxes[numeric_index]);
	}
}

template <AlignedIndexMode Mode, duckdb::idx_t Banks, duckdb::idx_t Unroll, bool SimplifyDerived>
inline void ConsumeAlignedFixedSlotChunk(const AlignedAggView &view, duckdb::idx_t count,
                                         ReusableFixedSlotCache &cache) {
	duckdb::idx_t row = 0;
	if constexpr (Unroll > 1) {
		const auto limit = count - (count % Unroll);
		for (; row < limit; row += Unroll) {
			ConsumeAlignedFixedSlotRows<Mode, Banks, SimplifyDerived>(view, row, row + Unroll, cache);
		}
	}
	ConsumeAlignedFixedSlotRows<Mode, Banks, SimplifyDerived>(view, row, count, cache);
}

template <AlignedIndexMode Mode>
void AggregateAlignedFixedSlotMode(const AlignedAggView &view, duckdb::idx_t count, ReusableFixedSlotCache &cache,
                                   std::vector<uint16_t> &used_keys, const FusedHotLoopConfig &config) {
	const auto banks = static_cast<duckdb::idx_t>(config.fixed_slot_banks);
	const auto unroll = config.fixed_slot_unroll;
	const auto simplify = config.simplify_exact_derived;
	if (banks == 8 && unroll == 8) {
		if (simplify) {
			ConsumeAlignedFixedSlotChunk<Mode, 8, 8, true>(view, count, cache);
		} else {
			ConsumeAlignedFixedSlotChunk<Mode, 8, 8, false>(view, count, cache);
		}
	} else if (banks == 4 && unroll >= 4) {
		if (simplify) {
			ConsumeAlignedFixedSlotChunk<Mode, 4, 4, true>(view, count, cache);
		} else {
			ConsumeAlignedFixedSlotChunk<Mode, 4, 4, false>(view, count, cache);
		}
	} else if (banks == 2) {
		if (simplify) {
			ConsumeAlignedFixedSlotChunk<Mode, 2, 1, true>(view, count, cache);
		} else {
			ConsumeAlignedFixedSlotChunk<Mode, 2, 1, false>(view, count, cache);
		}
	} else if (simplify) {
		ConsumeAlignedFixedSlotChunk<Mode, 1, 1, true>(view, count, cache);
	} else {
		ConsumeAlignedFixedSlotChunk<Mode, 1, 1, false>(view, count, cache);
	}
	MergeFixedSlotBanks(used_keys, cache, config.use_group_int64_partials, banks);
}

void AggregateAlignedFixedSlot(const AlignedAggView &view, duckdb::idx_t count, ReusableFixedSlotCache &cache,
                               std::vector<uint16_t> &used_keys, const FusedHotLoopConfig &config,
                               duckdb::idx_t &index_ns, duckdb::idx_t &update_ns, bool profile_split) {
	index_ns = 0;
	update_ns = 0;
	if (!profile_split) {
		switch (view.index_mode) {
		case AlignedIndexMode::IdentityFilter:
			AggregateAlignedFixedSlotMode<AlignedIndexMode::IdentityFilter>(view, count, cache, used_keys, config);
			break;
		case AlignedIndexMode::Nested:
			AggregateAlignedFixedSlotMode<AlignedIndexMode::Nested>(view, count, cache, used_keys, config);
			break;
		case AlignedIndexMode::Composed:
			AggregateAlignedFixedSlotMode<AlignedIndexMode::Composed>(view, count, cache, used_keys, config);
			break;
		}
		return;
	}

	// Profile path: materialize indices into scratch, then apply updates (measures index vs update).
	thread_local std::array<duckdb::sel_t, STANDARD_VECTOR_SIZE> scratch_numeric;
	thread_local std::array<duckdb::sel_t, STANDARD_VECTOR_SIZE> scratch_rf;
	thread_local std::array<duckdb::sel_t, STANDARD_VECTOR_SIZE> scratch_ls;
	const auto index_start = std::chrono::steady_clock::now();
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto payload_index = AlignedPayloadIndex(view, row);
		scratch_numeric[row] = static_cast<duckdb::sel_t>(payload_index);
		scratch_rf[row] = static_cast<duckdb::sel_t>(AlignedReturnflagIndex(view, row, payload_index));
		scratch_ls[row] = static_cast<duckdb::sel_t>(AlignedLinestatusIndex(view, row, payload_index));
	}
	index_ns = static_cast<duckdb::idx_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - index_start).count());

	const auto update_start = std::chrono::steady_clock::now();
	const auto ls_dict_size = cache.linestatus_dictionary_size;
	const auto simplify = config.simplify_exact_derived;
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto numeric_index = scratch_numeric[row];
		const auto pair_index = scratch_rf[row] * ls_dict_size + scratch_ls[row];
		const auto slot = cache.fixed_pair_to_slot[pair_index];
		auto &local = cache.fixed_slot_bank_states[0][slot];
		if (simplify) {
			UpdateChunkLocalState<true>(local, view.quantities[numeric_index], view.extendedprices[numeric_index],
			                            view.discounts[numeric_index], view.taxes[numeric_index]);
		} else {
			UpdateChunkLocalState<false>(local, view.quantities[numeric_index], view.extendedprices[numeric_index],
			                             view.discounts[numeric_index], view.taxes[numeric_index]);
		}
	}
	MergeFixedSlotBanks(used_keys, cache, config.use_group_int64_partials, 1);
	update_ns = static_cast<duckdb::idx_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - update_start).count());
}

void AggregateFlat(duckdb::Vector &quantity_vec, duckdb::Vector &extendedprice_vec, duckdb::Vector &discount_vec,
                   duckdb::Vector &tax_vec, duckdb::Vector &returnflag_vec, duckdb::Vector &linestatus_vec,
                   duckdb::idx_t count, std::array<GroupState, 1u << 16> &groups, std::vector<uint16_t> &used_keys,
                   const FusedHotLoopConfig &config) {
	auto quantities = duckdb::FlatVector::GetData<int64_t>(quantity_vec);
	auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vec);
	auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vec);
	auto taxes = duckdb::FlatVector::GetData<int64_t>(tax_vec);
	auto returnflags = duckdb::FlatVector::GetData<duckdb::string_t>(returnflag_vec);
	auto linestatuses = duckdb::FlatVector::GetData<duckdb::string_t>(linestatus_vec);
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto key = PackGroupKeyUnchecked(returnflags[row], linestatuses[row]);
		UpdateGroupDirect(groups[key], used_keys, key, quantities[row], extendedprices[row], discounts[row], taxes[row],
		                  config.use_group_int64_partials, config.simplify_exact_derived);
	}
}

void AggregateScalar(duckdb::Vector &quantity_vec, duckdb::Vector &extendedprice_vec, duckdb::Vector &discount_vec,
                     duckdb::Vector &tax_vec, duckdb::Vector &returnflag_vec, duckdb::Vector &linestatus_vec,
                     duckdb::idx_t count, std::array<GroupState, 1u << 16> &groups, std::vector<uint16_t> &used_keys,
                     const FusedHotLoopConfig &config) {
	duckdb::UnifiedVectorFormat quantity_data;
	duckdb::UnifiedVectorFormat extendedprice_data;
	duckdb::UnifiedVectorFormat discount_data;
	duckdb::UnifiedVectorFormat tax_data;
	duckdb::UnifiedVectorFormat returnflag_data;
	duckdb::UnifiedVectorFormat linestatus_data;
	quantity_vec.ToUnifiedFormat(count, quantity_data);
	extendedprice_vec.ToUnifiedFormat(count, extendedprice_data);
	discount_vec.ToUnifiedFormat(count, discount_data);
	tax_vec.ToUnifiedFormat(count, tax_data);
	returnflag_vec.ToUnifiedFormat(count, returnflag_data);
	linestatus_vec.ToUnifiedFormat(count, linestatus_data);

	auto *quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_data);
	auto *extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_data);
	auto *discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_data);
	auto *taxes = duckdb::UnifiedVectorFormat::GetData<int64_t>(tax_data);
	auto *returnflags = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(returnflag_data);
	auto *linestatuses = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(linestatus_data);

	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto q_idx = quantity_data.sel->get_index(row);
		const auto e_idx = extendedprice_data.sel->get_index(row);
		const auto d_idx = discount_data.sel->get_index(row);
		const auto t_idx = tax_data.sel->get_index(row);
		const auto rf_idx = returnflag_data.sel->get_index(row);
		const auto ls_idx = linestatus_data.sel->get_index(row);
		if (!quantity_data.validity.RowIsValid(q_idx) || !extendedprice_data.validity.RowIsValid(e_idx) ||
		    !discount_data.validity.RowIsValid(d_idx) || !tax_data.validity.RowIsValid(t_idx) ||
		    !returnflag_data.validity.RowIsValid(rf_idx) || !linestatus_data.validity.RowIsValid(ls_idx)) {
			continue;
		}
		const auto key = PackGroupKey(returnflags[rf_idx], linestatuses[ls_idx]);
		UpdateGroupDirect(groups[key], used_keys, key, quantities[q_idx], extendedprices[e_idx], discounts[d_idx],
		                  taxes[t_idx], config.use_group_int64_partials, config.simplify_exact_derived);
	}
}

//! Scalar aggregate over Scan-without-Slice vectors: `filter_sel` picks qualifying rows from `scan_count`.
void AggregateScalarFiltered(duckdb::Vector &quantity_vec, duckdb::Vector &extendedprice_vec,
                             duckdb::Vector &discount_vec, duckdb::Vector &tax_vec, duckdb::Vector &returnflag_vec,
                             duckdb::Vector &linestatus_vec, duckdb::SelectionVector &filter_sel, duckdb::idx_t count,
                             duckdb::idx_t scan_count, std::array<GroupState, 1u << 16> &groups,
                             std::vector<uint16_t> &used_keys, const FusedHotLoopConfig &config) {
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

	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto base = filter_sel.get_index(row);
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
		UpdateGroupDirect(groups[key], used_keys, key, quantities[q_idx], extendedprices[e_idx], discounts[d_idx],
		                  taxes[t_idx], config.use_group_int64_partials, config.simplify_exact_derived);
	}
}

using Clock = std::chrono::steady_clock;

} // namespace

bool Q1FusedScanEnabled() {
#ifdef ACCEL_EXPERIMENTAL_Q1_OPTS
	return ReadBoolEnv("ACCEL_Q1_FUSED_SCAN", true);
#else
	return true;
#endif
}

bool Q1FusedAlignedPayloadEnabled() {
#ifdef ACCEL_EXPERIMENTAL_Q1_OPTS
	return ReadBoolEnv("ACCEL_Q1_FUSED_ALIGNED_PAYLOAD", true);
#else
	return true;
#endif
}

bool Q1FusedDirectIndexEnabled() {
#ifdef ACCEL_EXPERIMENTAL_Q1_OPTS
	if (ReadBoolEnv("ACCEL_Q1_FUSED_COMPOSE_SEL", false)) {
		return false;
	}
	return ReadBoolEnv("ACCEL_Q1_FUSED_DIRECT_INDEX", true);
#else
	return true;
#endif
}

Q1FusedAggMode GetQ1FusedAggMode() {
#ifdef ACCEL_EXPERIMENTAL_Q1_OPTS
	if (ReadBoolEnv("ACCEL_Q1_FUSED_SCALAR", false)) {
		return Q1FusedAggMode::ScalarOnly;
	}
	if (ReadBoolEnv("ACCEL_Q1_FUSED_FIXED_SLOT", true)) {
		return Q1FusedAggMode::FixedSlotPreferred;
	}
	return Q1FusedAggMode::ScalarOnly;
#else
	return Q1FusedAggMode::FixedSlotPreferred;
#endif
}

struct Q1FusedKernel::State {
	std::array<GroupState, 1u << 16> groups {};
	std::vector<uint16_t> used_keys;
	std::vector<OutputRow> ordered_rows;
	ReusableFixedSlotCache fixed_slot_cache;
	duckdb::SelectionVector composed_returnflag_sel;
	duckdb::SelectionVector composed_linestatus_sel;
	bool scanned = false;
	bool finalized = false;
	duckdb::idx_t output_offset = 0;

	State() : composed_returnflag_sel(STANDARD_VECTOR_SIZE), composed_linestatus_sel(STANDARD_VECTOR_SIZE) {
	}
};

Q1FusedKernel::Q1FusedKernel(Q1Parameters params_p)
    : params(std::move(params_p)), state(std::make_unique<State>()) {
	diagnostics.agg_mode = GetQ1FusedAggMode();
}

Q1FusedKernel::~Q1FusedKernel() = default;

void Q1FusedKernel::ConsumeInput(duckdb::idx_t, duckdb::DataChunk &) {
	throw duckdb::InternalException("Q1FusedKernel does not consume DataChunks; call RunFusedScan");
}

void Q1FusedKernel::RunFusedScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
	if (state->scanned) {
		return;
	}
	if (!NativeScanHasFilters(bind_data)) {
		diagnostics.refused_missing_filters = true;
		throw duckdb::InvalidInputException(
		    "Q1 fused scan requires a pushed shipdate filter; refusing TrustFilter without native filters");
	}

	diagnostics.used_fused_path = true;
	diagnostics.residual_checks = 0;
	diagnostics.chunks_materialized = 0;
	diagnostics.agg_mode = GetQ1FusedAggMode();
	const auto &hot_loop_config = GetFusedHotLoopConfig();
	const bool force_scalar = diagnostics.agg_mode == Q1FusedAggMode::ScalarOnly;
	const bool use_aligned_payload = Q1FusedAlignedPayloadEnabled();
	const bool force_compose_sel = !Q1FusedDirectIndexEnabled();
	const bool profile_sel =
#ifdef ACCEL_EXPERIMENTAL_Q1_OPTS
	    ReadBoolEnv("ACCEL_Q1_FUSED_SEL_PROFILE", false);
#else
	    false;
#endif
	diagnostics.used_aligned_payload = use_aligned_payload;

	static constexpr duckdb::idx_t kPayloadScanIndices[] = {
	    kQuantityScanIndex, kExtendedpriceScanIndex, kDiscountScanIndex,
	    kTaxScanIndex,      kReturnflagScanIndex,    kLinestatusScanIndex,
	};

	auto consumer = [this, &hot_loop_config, force_scalar, use_aligned_payload, force_compose_sel, profile_sel](
	                    duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
	                    duckdb::RowGroup &row_group, duckdb::SelectionVector &sel,
	                    duckdb::idx_t count) -> duckdb::FilteredScanConsumerResult {
		diagnostics.vectors_consumed += 1;
		diagnostics.qualifying_rows += count;

		const auto &column_ids = scan_state.GetColumnIds();
		if (column_ids.size() != kProjectedColumnCount) {
			throw duckdb::InternalException("Q1 fused scan expected %llu projected columns, got %llu",
			                                static_cast<unsigned long long>(kProjectedColumnCount),
			                                static_cast<unsigned long long>(column_ids.size()));
		}

		duckdb::Vector quantity_vec(duckdb::LogicalType::DECIMAL(15, 2));
		duckdb::Vector extendedprice_vec(duckdb::LogicalType::DECIMAL(15, 2));
		duckdb::Vector discount_vec(duckdb::LogicalType::DECIMAL(15, 2));
		duckdb::Vector tax_vec(duckdb::LogicalType::DECIMAL(15, 2));
		duckdb::Vector returnflag_vec(duckdb::LogicalType::VARCHAR);
		duckdb::Vector linestatus_vec(duckdb::LogicalType::VARCHAR);

		duckdb::idx_t scan_count = count;
		const auto decode_start = Clock::now();
		if (use_aligned_payload) {
			duckdb::Vector *payload_results[] = {&quantity_vec, &extendedprice_vec, &discount_vec,
			                                     &tax_vec,      &returnflag_vec,    &linestatus_vec};
			scan_count = row_group.ScanProjectedColumns(transaction, scan_state, kPayloadScanIndices,
			                                            /*column_count=*/6, payload_results,
			                                            duckdb::UpdateScanType::STANDARD);
			diagnostics.aligned_payload_vectors += 1;
			diagnostics.aligned_payload_rows += count;
		} else {
			auto select_column = [&](duckdb::idx_t scan_idx, duckdb::Vector &out) {
				row_group.SelectProjectedColumn(transaction, scan_state, scan_idx, sel, count, out,
				                                duckdb::UpdateScanType::STANDARD);
			};
			select_column(kQuantityScanIndex, quantity_vec);
			select_column(kExtendedpriceScanIndex, extendedprice_vec);
			select_column(kDiscountScanIndex, discount_vec);
			select_column(kTaxScanIndex, tax_vec);
			select_column(kReturnflagScanIndex, returnflag_vec);
			select_column(kLinestatusScanIndex, linestatus_vec);
		}
		const auto decode_ns = static_cast<duckdb::idx_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - decode_start).count());
		diagnostics.payload_decode_ns += decode_ns;
		diagnostics.payload_select_ns += decode_ns;

		RecordLayout(diagnostics.layouts, quantity_vec, extendedprice_vec, discount_vec, tax_vec, returnflag_vec,
		             linestatus_vec, use_aligned_payload ? scan_count : count);

		const auto agg_start = Clock::now();
		bool consumed = false;
		if (use_aligned_payload) {
			if (!force_scalar) {
				AlignedAggView aligned_view;
				duckdb::idx_t compose_ns = 0;
				if (TryGetAlignedAggView(quantity_vec, extendedprice_vec, discount_vec, tax_vec, returnflag_vec,
				                         linestatus_vec, sel, count, scan_count, force_compose_sel,
				                         state->composed_returnflag_sel, state->composed_linestatus_sel, aligned_view,
				                         compose_ns)) {
					diagnostics.sel_compose_ns += compose_ns;
					switch (aligned_view.index_mode) {
					case AlignedIndexMode::IdentityFilter:
						diagnostics.aligned_identity_filter_vectors += 1;
						break;
					case AlignedIndexMode::Nested:
						diagnostics.aligned_nested_index_vectors += 1;
						break;
					case AlignedIndexMode::Composed:
						diagnostics.aligned_compose_sel_vectors += 1;
						break;
					}
					if (!CanReuseAlignedFixedSlotCache(aligned_view, state->fixed_slot_cache, returnflag_vec,
					                                   linestatus_vec)) {
						if (RebuildAlignedFixedSlotCache(returnflag_vec, linestatus_vec, aligned_view, count,
						                                 state->groups, state->fixed_slot_cache)) {
							duckdb::idx_t index_ns = 0;
							duckdb::idx_t update_ns = 0;
							AggregateAlignedFixedSlot(aligned_view, count, state->fixed_slot_cache, state->used_keys,
							                          hot_loop_config, index_ns, update_ns, profile_sel);
							diagnostics.sel_index_ns += index_ns;
							diagnostics.agg_update_ns += update_ns;
							diagnostics.fixed_slot_vectors += 1;
							diagnostics.fixed_slot_rows += count;
							diagnostics.aligned_fixed_slot_vectors += 1;
							diagnostics.aligned_fixed_slot_rows += count;
							consumed = true;
						}
					} else {
						ResetFixedSlotBankStates(state->fixed_slot_cache);
						duckdb::idx_t index_ns = 0;
						duckdb::idx_t update_ns = 0;
						AggregateAlignedFixedSlot(aligned_view, count, state->fixed_slot_cache, state->used_keys,
						                          hot_loop_config, index_ns, update_ns, profile_sel);
						diagnostics.sel_index_ns += index_ns;
						diagnostics.agg_update_ns += update_ns;
						diagnostics.fixed_slot_vectors += 1;
						diagnostics.fixed_slot_rows += count;
						diagnostics.aligned_fixed_slot_vectors += 1;
						diagnostics.aligned_fixed_slot_rows += count;
						consumed = true;
					}
				}
			}
			if (!consumed) {
				AggregateScalarFiltered(quantity_vec, extendedprice_vec, discount_vec, tax_vec, returnflag_vec,
				                        linestatus_vec, sel, count, scan_count, state->groups, state->used_keys,
				                        hot_loop_config);
				diagnostics.scalar_fallback_vectors += 1;
				diagnostics.scalar_fallback_rows += count;
			}
		} else if (!force_scalar) {
			FusedDominantView dominant_view;
			if (TryGetFusedDominantView(quantity_vec, extendedprice_vec, discount_vec, tax_vec, returnflag_vec,
			                            linestatus_vec, count, dominant_view)) {
				if (!CanReuseFixedSlotCache(dominant_view, state->fixed_slot_cache, returnflag_vec, linestatus_vec,
				                            count)) {
					if (RebuildFixedSlotCache(returnflag_vec, linestatus_vec, dominant_view, count, state->groups,
					                          state->fixed_slot_cache)) {
						AggregateFixedSlot(dominant_view, count, state->fixed_slot_cache, state->used_keys,
						                   hot_loop_config);
						diagnostics.fixed_slot_vectors += 1;
						diagnostics.fixed_slot_rows += count;
						consumed = true;
					}
				} else {
					ResetFixedSlotBankStates(state->fixed_slot_cache);
					AggregateFixedSlot(dominant_view, count, state->fixed_slot_cache, state->used_keys, hot_loop_config);
					diagnostics.fixed_slot_vectors += 1;
					diagnostics.fixed_slot_rows += count;
					consumed = true;
				}
			} else if (IsFlatAllValid(quantity_vec) && IsFlatAllValid(extendedprice_vec) &&
			           IsFlatAllValid(discount_vec) && IsFlatAllValid(tax_vec) && IsFlatAllValid(returnflag_vec) &&
			           IsFlatAllValid(linestatus_vec)) {
				AggregateFlat(quantity_vec, extendedprice_vec, discount_vec, tax_vec, returnflag_vec, linestatus_vec,
				              count, state->groups, state->used_keys, hot_loop_config);
				diagnostics.flat_fast_vectors += 1;
				diagnostics.flat_fast_rows += count;
				consumed = true;
			}
			if (!consumed) {
				AggregateScalar(quantity_vec, extendedprice_vec, discount_vec, tax_vec, returnflag_vec, linestatus_vec,
				                count, state->groups, state->used_keys, hot_loop_config);
				diagnostics.scalar_fallback_vectors += 1;
				diagnostics.scalar_fallback_rows += count;
			}
		} else {
			AggregateScalar(quantity_vec, extendedprice_vec, discount_vec, tax_vec, returnflag_vec, linestatus_vec,
			                count, state->groups, state->used_keys, hot_loop_config);
			diagnostics.scalar_fallback_vectors += 1;
			diagnostics.scalar_fallback_rows += count;
		}

		diagnostics.aggregate_ns += static_cast<duckdb::idx_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - agg_start).count());
		(void)kShipdateScanIndex;
		return duckdb::FilteredScanConsumerResult::CONTINUE;
	};

	if (!RunNativeFilteredScanConsumer(context, bind_data, consumer)) {
		throw duckdb::InterruptException();
	}
	state->scanned = true;
}

void Q1FusedKernel::Finalize() {
	if (state->finalized) {
		return;
	}
	if (!state->scanned) {
		throw duckdb::InternalException("Q1FusedKernel::Finalize called before RunFusedScan");
	}
	const auto use_partials = GetFusedHotLoopConfig().use_group_int64_partials;
	state->ordered_rows.reserve(state->used_keys.size());
	for (const auto key : state->used_keys) {
		auto &group = state->groups[key];
		if (use_partials) {
			FlushGroupPendingPartials(group);
		}
		OutputRow row {key,
		               FromWideHugeint(group.sum_qty),
		               FromWideHugeint(group.sum_base_price),
		               FromWideHugeint(group.sum_disc_price),
		               FromWideHugeint(group.sum_charge),
		               HugeintScaleToDouble(group.sum_qty, kDecimalScale2, group.count),
		               HugeintScaleToDouble(group.sum_base_price, kDecimalScale2, group.count),
		               HugeintScaleToDouble(group.sum_discount, kDecimalScale2, group.count),
		               static_cast<int64_t>(group.count)};
		state->ordered_rows.push_back(row);
	}
	std::sort(state->ordered_rows.begin(), state->ordered_rows.end(),
	          [](const OutputRow &lhs, const OutputRow &rhs) { return lhs.key < rhs.key; });
	state->finalized = true;
}

bool Q1FusedKernel::ProduceOutput(duckdb::DataChunk &output) {
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

duckdb::unique_ptr<KernelInstance> CreateQ1FusedKernel(const Q1Parameters &params) {
	return duckdb::make_uniq<Q1FusedKernel>(params);
}

} // namespace accelerator_codegen
