#include "accelerator_codegen/tpch_q1.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {

namespace {

constexpr uint8_t kDecimalScale2 = 2;
constexpr uint8_t kDecimalScale4 = 4;
constexpr uint8_t kDecimalScale6 = 6;
constexpr uint8_t kDecimalWidth38 = 38;
constexpr duckdb::idx_t kQ1OutputBatchSize = 2;
constexpr duckdb::idx_t kChunkLocalGroupReserve = 16;
constexpr duckdb::idx_t kFixedSlotComboLimit = 256;
constexpr duckdb::idx_t kFixedSlotStateLimit = 16;
constexpr duckdb::idx_t kMaxFixedSlotBanks = 8;

#if defined(__SIZEOF_INT128__)
using wide_hugeint_t = __int128_t;
using q1_acc_t = wide_hugeint_t;

duckdb::hugeint_t FromWideHugeint(wide_hugeint_t value) {
    return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}

// Safe for DuckDB's bundled TPC-H DBGEN domains:
// - L_QTY_MAX = 50, L_DCNT_MAX = 10, L_TAX_MAX = 8 from dss.h
// - retail price max is 209900 scaled cents from rpb_routine()
// - lineitem extended price is retailprice * quantity / 100, so eprice_raw <= 104950
// - the default vector size is 2048, so the worst per-chunk raw sum_charge is below 2.4e12
// - TPC-H lineitems are <= 10.5M * SF, which keeps even SF=100000 well below signed __int128 capacity
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
        throw duckdb::OutOfRangeException("Overflow while accumulating Q1 aggregate");
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

bool FlatChunkFlagsAreSingleChar(duckdb::string_t *returnflags, duckdb::string_t *linestatuses, duckdb::idx_t count) {
    for (duckdb::idx_t row = 0; row < count; row++) {
        if (returnflags[row].GetSize() != 1 || linestatuses[row].GetSize() != 1) {
            return false;
        }
    }
    return true;
}

struct DominantDictionaryChunkView {
    const duckdb::SelectionVector *numeric_sel = nullptr;
    const duckdb::SelectionVector *returnflag_sel = nullptr;
    const duckdb::SelectionVector *linestatus_sel = nullptr;
    const duckdb::date_t *shipdates = nullptr;
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

struct DominantPackedKeyCache {
    duckdb::idx_t returnflag_dictionary_size = 0;
    duckdb::idx_t linestatus_dictionary_size = 0;
    std::vector<uint16_t> packed_keys;
};

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

bool DictionaryChunkFlagsAreSingleChar(const duckdb::SelectionVector &returnflag_sel,
                                       const duckdb::ValidityMask &returnflag_validity,
                                       const duckdb::string_t *returnflags,
                                       const duckdb::SelectionVector &linestatus_sel,
                                       const duckdb::ValidityMask &linestatus_validity,
                                       const duckdb::string_t *linestatuses,
                                       duckdb::idx_t count) {
    const bool returnflag_all_valid = returnflag_validity.AllValid();
    const bool linestatus_all_valid = linestatus_validity.AllValid();
    for (duckdb::idx_t row = 0; row < count; row++) {
        const auto returnflag_index = returnflag_sel.get_index(row);
        const auto linestatus_index = linestatus_sel.get_index(row);
        if ((!returnflag_all_valid && !returnflag_validity.RowIsValidUnsafe(returnflag_index)) ||
            (!linestatus_all_valid && !linestatus_validity.RowIsValidUnsafe(linestatus_index)) ||
            returnflags[returnflag_index].GetSize() != 1 || linestatuses[linestatus_index].GetSize() != 1) {
            return false;
        }
    }
    return true;
}

bool TryGetDominantDictionaryChunkView(duckdb::DataChunk &input, DominantDictionaryChunkView &view) {
    duckdb::Vector *numeric_vectors[] = {
        &input.data[Q1LineitemChunkLayout::SHIPDATE_INDEX],
        &input.data[Q1LineitemChunkLayout::QUANTITY_INDEX],
        &input.data[Q1LineitemChunkLayout::EXTENDEDPRICE_INDEX],
        &input.data[Q1LineitemChunkLayout::DISCOUNT_INDEX],
        &input.data[Q1LineitemChunkLayout::TAX_INDEX],
    };

    const duckdb::SelectionVector *numeric_sel = nullptr;
    for (auto *vector : numeric_vectors) {
        if (!IsDictionaryOverFlatAllValid(*vector)) {
            return false;
        }
        auto &sel = duckdb::DictionaryVector::SelVector(*vector);
        if (!numeric_sel) {
            numeric_sel = &sel;
        } else if (numeric_sel->data() != sel.data()) {
            return false;
        }
    }

    auto &returnflag_vector = input.data[Q1LineitemChunkLayout::RETURNFLAG_INDEX];
    auto &linestatus_vector = input.data[Q1LineitemChunkLayout::LINESTATUS_INDEX];
    if (returnflag_vector.GetVectorType() != duckdb::VectorType::DICTIONARY_VECTOR ||
        linestatus_vector.GetVectorType() != duckdb::VectorType::DICTIONARY_VECTOR) {
        return false;
    }
    auto &returnflag_child = duckdb::DictionaryVector::Child(returnflag_vector);
    auto &linestatus_child = duckdb::DictionaryVector::Child(linestatus_vector);
    if (returnflag_child.GetVectorType() != duckdb::VectorType::FLAT_VECTOR ||
        linestatus_child.GetVectorType() != duckdb::VectorType::FLAT_VECTOR) {
        return false;
    }

    auto &returnflag_sel = duckdb::DictionaryVector::SelVector(returnflag_vector);
    auto &linestatus_sel = duckdb::DictionaryVector::SelVector(linestatus_vector);
    auto returnflags = GetFlatChildData<duckdb::string_t>(returnflag_vector);
    auto linestatuses = GetFlatChildData<duckdb::string_t>(linestatus_vector);
    auto &returnflag_validity = duckdb::FlatVector::Validity(returnflag_child);
    auto &linestatus_validity = duckdb::FlatVector::Validity(linestatus_child);
    view.numeric_sel = numeric_sel;
    view.returnflag_sel = &returnflag_sel;
    view.linestatus_sel = &linestatus_sel;
    view.shipdates = GetFlatChildData<duckdb::date_t>(input.data[Q1LineitemChunkLayout::SHIPDATE_INDEX]);
    view.quantities = GetFlatChildData<int64_t>(input.data[Q1LineitemChunkLayout::QUANTITY_INDEX]);
    view.extendedprices = GetFlatChildData<int64_t>(input.data[Q1LineitemChunkLayout::EXTENDEDPRICE_INDEX]);
    view.discounts = GetFlatChildData<int64_t>(input.data[Q1LineitemChunkLayout::DISCOUNT_INDEX]);
    view.taxes = GetFlatChildData<int64_t>(input.data[Q1LineitemChunkLayout::TAX_INDEX]);
    view.returnflags = returnflags;
    view.linestatuses = linestatuses;
    view.returnflag_validity = &returnflag_validity;
    view.linestatus_validity = &linestatus_validity;
    view.returnflag_all_valid = returnflag_validity.AllValid();
    view.linestatus_all_valid = linestatus_validity.AllValid();
    return true;
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

struct Q1HotLoopConfig {
    uint8_t fixed_slot_banks = 1;
    uint8_t fixed_slot_unroll = 1;
    bool use_group_int64_partials = false;
    bool simplify_exact_derived = false;
    bool trust_native_filter = false;
};

int ReadPositiveEnvInt(const char *name, int fallback) {
    const char *value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    const auto parsed = std::atoi(value);
    return parsed > 0 ? parsed : fallback;
}

bool ReadBoolEnv(const char *name, bool fallback) {
    const char *value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    return value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y';
}

Q1HotLoopConfig LoadQ1HotLoopConfig() {
    Q1HotLoopConfig config;
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
    config.trust_native_filter = ReadBoolEnv("ACCEL_Q1_TRUST_NATIVE_FILTER", false);
    return config;
}

const Q1HotLoopConfig &GetQ1HotLoopConfig() {
    static const auto config = LoadQ1HotLoopConfig();
    return config;
}

duckdb::idx_t DictionarySizeOrReferencedSpan(const duckdb::Vector &vector,
                                             const duckdb::SelectionVector &sel,
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

bool BuildDominantStringByteCache(duckdb::DataChunk &input,
                                  const DominantDictionaryChunkView &view,
                                  DominantStringByteCache &cache,
                                  uint64_t *checksum = nullptr) {
    cache.returnflag_dictionary_size = DictionarySizeOrReferencedSpan(
        input.data[Q1LineitemChunkLayout::RETURNFLAG_INDEX], *view.returnflag_sel, input.size());
    cache.linestatus_dictionary_size = DictionarySizeOrReferencedSpan(
        input.data[Q1LineitemChunkLayout::LINESTATUS_INDEX], *view.linestatus_sel, input.size());
    cache.returnflag_chars.assign(cache.returnflag_dictionary_size, 0);
    cache.linestatus_chars.assign(cache.linestatus_dictionary_size, 0);

    std::vector<uint8_t> returnflag_seen(cache.returnflag_dictionary_size, 0);
    std::vector<uint8_t> linestatus_seen(cache.linestatus_dictionary_size, 0);
    uint64_t local_checksum = 0;

    for (duckdb::idx_t row = 0; row < input.size(); row++) {
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
            const auto value = static_cast<uint8_t>(returnflag.GetDataUnsafe()[0]);
            cache.returnflag_chars[returnflag_index] = value;
            returnflag_seen[returnflag_index] = 1;
            local_checksum += value;
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
            const auto value = static_cast<uint8_t>(linestatus.GetDataUnsafe()[0]);
            cache.linestatus_chars[linestatus_index] = value;
            linestatus_seen[linestatus_index] = 1;
            local_checksum += static_cast<uint64_t>(value) << 8;
        }
    }

    if (checksum) {
        *checksum += local_checksum;
    }
    return true;
}

bool BuildDominantStringByteCacheReusingScratch(duckdb::DataChunk &input,
                                                const DominantDictionaryChunkView &view,
                                                DominantStringByteCache &cache,
                                                std::vector<uint8_t> &returnflag_seen,
                                                std::vector<uint8_t> &linestatus_seen) {
    cache.returnflag_dictionary_size = DictionarySizeOrReferencedSpan(
        input.data[Q1LineitemChunkLayout::RETURNFLAG_INDEX], *view.returnflag_sel, input.size());
    cache.linestatus_dictionary_size = DictionarySizeOrReferencedSpan(
        input.data[Q1LineitemChunkLayout::LINESTATUS_INDEX], *view.linestatus_sel, input.size());
    cache.returnflag_chars.resize(cache.returnflag_dictionary_size);
    cache.linestatus_chars.resize(cache.linestatus_dictionary_size);

    returnflag_seen.assign(cache.returnflag_dictionary_size, 0);
    linestatus_seen.assign(cache.linestatus_dictionary_size, 0);

    for (duckdb::idx_t row = 0; row < input.size(); row++) {
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

void BuildDominantPackedKeyCache(const DominantStringByteCache &byte_cache,
                                 DominantPackedKeyCache &packed_key_cache,
                                 uint64_t *checksum = nullptr) {
    packed_key_cache.returnflag_dictionary_size = byte_cache.returnflag_dictionary_size;
    packed_key_cache.linestatus_dictionary_size = byte_cache.linestatus_dictionary_size;
    packed_key_cache.packed_keys.assign(packed_key_cache.returnflag_dictionary_size *
                                            packed_key_cache.linestatus_dictionary_size,
                                        0);

    uint64_t local_checksum = 0;
    for (duckdb::idx_t returnflag_index = 0; returnflag_index < packed_key_cache.returnflag_dictionary_size;
         returnflag_index++) {
        const auto returnflag_char = byte_cache.returnflag_chars[returnflag_index];
        const auto base = returnflag_index * packed_key_cache.linestatus_dictionary_size;
        for (duckdb::idx_t linestatus_index = 0; linestatus_index < packed_key_cache.linestatus_dictionary_size;
             linestatus_index++) {
            const auto key =
                PackGroupKeyUncheckedBytes(returnflag_char, byte_cache.linestatus_chars[linestatus_index]);
            packed_key_cache.packed_keys[base + linestatus_index] = key;
            local_checksum += key;
        }
    }
    if (checksum) {
        *checksum += local_checksum;
    }
}

std::string UnpackLeft(uint16_t key) {
    return std::string(1, static_cast<char>(key >> 8));
}

std::string UnpackRight(uint16_t key) {
    return std::string(1, static_cast<char>(key & 0xFF));
}

struct Q1GroupState {
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

struct Q1OutputRow {
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

struct ReusableDominantPackedKeyChunkLocalCache {
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
    std::vector<uint16_t> pair_to_slot;
    std::vector<uint16_t> slot_keys;
    std::vector<Q1GroupState *> slot_targets;
    std::vector<ChunkLocalAggregateState> slot_states;
    std::vector<uint8_t> returnflag_seen;
    std::vector<uint8_t> linestatus_seen;
    std::array<uint8_t, kFixedSlotComboLimit> fixed_pair_to_slot {};
    std::array<uint16_t, kFixedSlotStateLimit> fixed_slot_keys {};
    std::array<Q1GroupState *, kFixedSlotStateLimit> fixed_slot_targets {};
    std::array<std::array<ChunkLocalAggregateState, kFixedSlotStateLimit>, kMaxFixedSlotBanks> fixed_slot_bank_states {};
    duckdb::idx_t fixed_combo_count = 0;
    duckdb::idx_t fixed_slot_count = 0;
};

struct ChunkLocalAggregateBuffer {
    std::array<uint16_t, kChunkLocalGroupReserve> keys {};
    std::array<ChunkLocalAggregateState, kChunkLocalGroupReserve> states {};
    duckdb::idx_t count = 0;

    ChunkLocalAggregateState &Get(uint16_t key) {
        for (duckdb::idx_t idx = 0; idx < count; idx++) {
            if (keys[idx] == key) {
                return states[idx];
            }
        }
        if (count >= kChunkLocalGroupReserve) {
            throw duckdb::InternalException("Q1 chunk-local group buffer exhausted");
        }
        keys[count] = key;
        states[count] = {};
        return states[count++];
    }
};

template <bool IncludeDerived, bool SimplifyDerived>
inline void UpdateChunkLocalState(ChunkLocalAggregateState &state,
                                  int64_t quantity_raw,
                                  int64_t extendedprice_raw,
                                  int64_t discount_raw,
                                  int64_t tax_raw) {
    state.sum_qty += quantity_raw;
    state.sum_base_price += extendedprice_raw;
    state.sum_discount += discount_raw;
    state.count += 1;
    if constexpr (IncludeDerived) {
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
}

inline void FlushGroupPendingPartials(Q1GroupState &state) {
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

void MergeChunkLocalIntoGroup(Q1GroupState &state, const ChunkLocalAggregateState &local, bool use_group_int64_partials) {
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
                throw duckdb::OutOfRangeException("Overflow while accumulating Q1 int64 partials");
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

template <bool IncludeDerived>
void MergeChunkLocalState(std::array<Q1GroupState, 1u << 16> &groups,
                          std::vector<uint16_t> &used_keys,
                          const ChunkLocalAggregateBuffer &buffer,
                          bool use_group_int64_partials) {
    for (duckdb::idx_t idx = 0; idx < buffer.count; idx++) {
        const auto key = buffer.keys[idx];
        const auto &local = buffer.states[idx];
        auto &state = groups[key];
        if (!state.used) {
            state.used = true;
            used_keys.push_back(key);
        }
        if constexpr (IncludeDerived) {
            MergeChunkLocalIntoGroup(state, local, use_group_int64_partials);
        } else {
            ChunkLocalAggregateState derived_free_local = local;
            derived_free_local.sum_disc_price = 0;
            derived_free_local.sum_charge = 0;
            MergeChunkLocalIntoGroup(state, derived_free_local, use_group_int64_partials);
        }
    }
}

bool RebuildReusableDominantPackedKeyChunkLocalCache(
    duckdb::DataChunk &input,
    const DominantDictionaryChunkView &view,
    std::array<Q1GroupState, 1u << 16> &groups,
    std::vector<uint16_t> &used_keys,
    ReusableDominantPackedKeyChunkLocalCache &cache) {
    if (!BuildDominantStringByteCacheReusingScratch(input, view, cache.byte_cache, cache.returnflag_seen,
                                                    cache.linestatus_seen)) {
        cache.valid = false;
        return false;
    }

    BuildDominantPackedKeyCache(cache.byte_cache, cache.packed_key_cache);
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

    cache.fixed_combo_count = combo_count;
    cache.fixed_slot_count = 0;
    cache.use_fixed_slots = true;
    cache.fixed_pair_to_slot.fill(static_cast<uint8_t>(0xFF));

    cache.pair_to_slot.assign(combo_count, static_cast<uint16_t>(-1));
    cache.slot_keys.clear();
    cache.slot_targets.clear();
    cache.slot_states.clear();
    cache.slot_keys.reserve(combo_count);
    cache.slot_targets.reserve(combo_count);
    cache.slot_states.resize(0);
    cache.slot_states.reserve(combo_count);

    for (duckdb::idx_t pair_index = 0; pair_index < combo_count; pair_index++) {
        const auto key = cache.packed_key_cache.packed_keys[pair_index];
        uint16_t slot = static_cast<uint16_t>(-1);
        for (uint16_t idx = 0; idx < cache.slot_keys.size(); idx++) {
            if (cache.slot_keys[idx] == key) {
                slot = idx;
                break;
            }
        }
        if (slot == static_cast<uint16_t>(-1)) {
            slot = static_cast<uint16_t>(cache.slot_keys.size());
            cache.slot_keys.push_back(key);
            cache.slot_targets.push_back(&groups[key]);
            cache.slot_states.push_back({});
        }
        cache.pair_to_slot[pair_index] = slot;

        if (cache.use_fixed_slots) {
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
                } else {
                    fixed_slot = static_cast<uint8_t>(cache.fixed_slot_count);
                    cache.fixed_slot_keys[cache.fixed_slot_count] = key;
                    cache.fixed_slot_targets[cache.fixed_slot_count] = &groups[key];
                    for (duckdb::idx_t bank = 0; bank < kMaxFixedSlotBanks; bank++) {
                        cache.fixed_slot_bank_states[bank][cache.fixed_slot_count] = {};
                    }
                    cache.fixed_slot_count += 1;
                }
            }
            if (cache.use_fixed_slots) {
                cache.fixed_pair_to_slot[pair_index] = fixed_slot;
            }
        }
    }
    cache.valid = true;
    return true;
}

bool CanReuseDominantPackedKeyChunkLocalCache(duckdb::DataChunk &input,
                                              const DominantDictionaryChunkView &view,
                                              const ReusableDominantPackedKeyChunkLocalCache &cache) {
    if (!cache.valid) {
        return false;
    }
    const auto returnflag_dictionary_size = DictionarySizeOrReferencedSpan(
        input.data[Q1LineitemChunkLayout::RETURNFLAG_INDEX], *view.returnflag_sel, input.size());
    const auto linestatus_dictionary_size = DictionarySizeOrReferencedSpan(
        input.data[Q1LineitemChunkLayout::LINESTATUS_INDEX], *view.linestatus_sel, input.size());
    return cache.returnflag_child == view.returnflags && cache.linestatus_child == view.linestatuses &&
           cache.returnflag_validity == view.returnflag_validity && cache.linestatus_validity == view.linestatus_validity &&
           cache.returnflag_all_valid == view.returnflag_all_valid &&
           cache.linestatus_all_valid == view.linestatus_all_valid &&
           cache.returnflag_dictionary_size == returnflag_dictionary_size &&
           cache.linestatus_dictionary_size == linestatus_dictionary_size;
}

void ResetReusableDominantPackedKeyChunkLocalCacheState(ReusableDominantPackedKeyChunkLocalCache &cache) {
    if (cache.use_fixed_slots) {
        for (duckdb::idx_t bank = 0; bank < kMaxFixedSlotBanks; bank++) {
            for (duckdb::idx_t idx = 0; idx < cache.fixed_slot_count; idx++) {
                cache.fixed_slot_bank_states[bank][idx] = {};
            }
        }
    }
    for (auto &state : cache.slot_states) {
        state = {};
    }
}

template <bool TrustFilter, duckdb::idx_t Banks, bool SimplifyDerived>
inline void ConsumeFixedSlotRowsScalar(const DominantDictionaryChunkView &view,
                                       duckdb::idx_t begin,
                                       duckdb::idx_t end,
                                       duckdb::date_t cutoff_date,
                                       ReusableDominantPackedKeyChunkLocalCache &cache) {
    for (duckdb::idx_t row = begin; row < end; row++) {
        const auto numeric_index = view.numeric_sel->get_index(row);
        if constexpr (!TrustFilter) {
            if (!(view.shipdates[numeric_index] <= cutoff_date)) {
                continue;
            }
        }
        const auto returnflag_index = view.returnflag_sel->get_index(row);
        const auto linestatus_index = view.linestatus_sel->get_index(row);
        const auto pair_index = returnflag_index * cache.linestatus_dictionary_size + linestatus_index;
        const auto slot = cache.fixed_pair_to_slot[pair_index];
        auto &local = cache.fixed_slot_bank_states[row & (Banks - 1)][slot];
        UpdateChunkLocalState<true, SimplifyDerived>(local, view.quantities[numeric_index],
                                                     view.extendedprices[numeric_index], view.discounts[numeric_index],
                                                     view.taxes[numeric_index]);
    }
}

template <bool TrustFilter, duckdb::idx_t Banks, duckdb::idx_t Unroll, bool SimplifyDerived>
inline void ConsumeReusableFixedSlotChunkLocals(const DominantDictionaryChunkView &view,
                                                duckdb::idx_t count,
                                                duckdb::date_t cutoff_date,
                                                ReusableDominantPackedKeyChunkLocalCache &cache) {
    duckdb::idx_t row = 0;
    if constexpr (Unroll > 1) {
        const auto limit = count - (count % Unroll);
        for (; row < limit; row += Unroll) {
            ConsumeFixedSlotRowsScalar<TrustFilter, Banks, SimplifyDerived>(view, row, row + Unroll, cutoff_date,
                                                                            cache);
        }
    }
    ConsumeFixedSlotRowsScalar<TrustFilter, Banks, SimplifyDerived>(view, row, count, cutoff_date, cache);
}

inline void MergeReusableFixedSlotChunkLocals(std::vector<uint16_t> &used_keys,
                                              ReusableDominantPackedKeyChunkLocalCache &cache,
                                              bool use_group_int64_partials,
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

Q1ScanDiagnostics InspectFlatChunk(duckdb::DataChunk &input, const Q1Parameters &parameters) {
    Q1ScanDiagnostics diagnostics;
    diagnostics.total_chunks = 1;
    diagnostics.total_rows = input.size();

    auto shipdates = duckdb::FlatVector::GetData<duckdb::date_t>(input.data[Q1LineitemChunkLayout::SHIPDATE_INDEX]);
    auto returnflags = duckdb::FlatVector::GetData<duckdb::string_t>(input.data[Q1LineitemChunkLayout::RETURNFLAG_INDEX]);
    auto linestatuses = duckdb::FlatVector::GetData<duckdb::string_t>(input.data[Q1LineitemChunkLayout::LINESTATUS_INDEX]);

    if (FlatChunkFlagsAreSingleChar(returnflags, linestatuses, input.size())) {
        diagnostics.flat_fast_path_chunks = 1;
        diagnostics.flat_fast_path_rows = input.size();
    }

    for (duckdb::idx_t row = 0; row < input.size(); row++) {
        if (shipdates[row] <= parameters.cutoff_date) {
            diagnostics.qualifying_rows += 1;
        }
    }
    return diagnostics;
}

Q1ScanDiagnostics InspectDominantDictionaryChunk(duckdb::DataChunk &input, const Q1Parameters &parameters) {
    Q1ScanDiagnostics diagnostics;
    diagnostics.total_chunks = 1;
    diagnostics.dictionary_fast_path_chunks = 1;
    diagnostics.total_rows = input.size();
    diagnostics.dictionary_fast_path_rows = input.size();

    DominantDictionaryChunkView view;
    if (!TryGetDominantDictionaryChunkView(input, view)) {
        throw duckdb::InternalException("expected dominant dictionary Q1 chunk view");
    }

    for (duckdb::idx_t row = 0; row < input.size(); row++) {
        const auto numeric_index = view.numeric_sel->get_index(row);
        if (view.shipdates[numeric_index] <= parameters.cutoff_date) {
            diagnostics.qualifying_rows += 1;
        }
    }
    return diagnostics;
}

Q1ScanDiagnostics InspectGenericChunk(duckdb::DataChunk &input, const Q1Parameters &parameters) {
    Q1ScanDiagnostics diagnostics;
    diagnostics.total_chunks = 1;
    diagnostics.total_rows = input.size();

    duckdb::UnifiedVectorFormat shipdate_format;
    duckdb::UnifiedVectorFormat returnflag_format;
    duckdb::UnifiedVectorFormat linestatus_format;
    input.data[Q1LineitemChunkLayout::SHIPDATE_INDEX].ToUnifiedFormat(input.size(), shipdate_format);
    input.data[Q1LineitemChunkLayout::RETURNFLAG_INDEX].ToUnifiedFormat(input.size(), returnflag_format);
    input.data[Q1LineitemChunkLayout::LINESTATUS_INDEX].ToUnifiedFormat(input.size(), linestatus_format);

    auto shipdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdate_format);
    auto returnflags = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(returnflag_format);
    auto linestatuses = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(linestatus_format);
    const bool shipdate_all_valid = shipdate_format.validity.AllValid();
    const bool returnflag_all_valid = returnflag_format.validity.AllValid();
    const bool linestatus_all_valid = linestatus_format.validity.AllValid();

    for (duckdb::idx_t row = 0; row < input.size(); row++) {
        const auto shipdate_index = shipdate_format.sel->get_index(row);
        const auto returnflag_index = returnflag_format.sel->get_index(row);
        const auto linestatus_index = linestatus_format.sel->get_index(row);

        if ((!shipdate_all_valid && !shipdate_format.validity.RowIsValidUnsafe(shipdate_index)) ||
            (!returnflag_all_valid && !returnflag_format.validity.RowIsValidUnsafe(returnflag_index)) ||
            (!linestatus_all_valid && !linestatus_format.validity.RowIsValidUnsafe(linestatus_index))) {
            continue;
        }
        if (returnflags[returnflag_index].GetSize() != 1 || linestatuses[linestatus_index].GetSize() != 1) {
            continue;
        }
        if (shipdates[shipdate_index] <= parameters.cutoff_date) {
            diagnostics.qualifying_rows += 1;
        }
    }
    return diagnostics;
}

class Q1Kernel final : public KernelInstance {
public:
    explicit Q1Kernel(Q1Parameters params_p) : params(std::move(params_p)), hot_loop_config(GetQ1HotLoopConfig()) {
    }

    void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
        if (input_id != 0) {
            throw duckdb::InternalException("Q1 kernel received unexpected input id %llu",
                                            static_cast<unsigned long long>(input_id));
        }

        if (Q1ChunkCanUseFlatFastPath(input)) {
            ConsumeFlatChunk(input);
            return;
        }

        DominantDictionaryChunkView dominant_view;
        if (TryGetDominantDictionaryChunkView(input, dominant_view)) {
            ConsumeDominantDictionaryChunk(input, dominant_view);
            return;
        }

        ConsumeGenericChunk(input);
    }

    void Finalize() override {
        if (finalized) {
            return;
        }
        ordered_rows.reserve(used_keys.size());
        for (const auto key : used_keys) {
            auto &state = groups[key];
            if (hot_loop_config.use_group_int64_partials) {
                FlushGroupPendingPartials(state);
            }
            Q1OutputRow row {key,
                             FromWideHugeint(state.sum_qty),
                             FromWideHugeint(state.sum_base_price),
                             FromWideHugeint(state.sum_disc_price),
                             FromWideHugeint(state.sum_charge),
                             HugeintScaleToDouble(state.sum_qty, kDecimalScale2, state.count),
                             HugeintScaleToDouble(state.sum_base_price, kDecimalScale2, state.count),
                             HugeintScaleToDouble(state.sum_discount, kDecimalScale2, state.count),
                             static_cast<int64_t>(state.count)};
            ordered_rows.push_back(row);
        }
        std::sort(ordered_rows.begin(), ordered_rows.end(),
                  [](const Q1OutputRow &lhs, const Q1OutputRow &rhs) { return lhs.key < rhs.key; });
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
    void DirectUpdateGroupByKey(uint16_t group_key,
                                int64_t quantity_raw,
                                int64_t extendedprice_raw,
                                int64_t discount_raw,
                                int64_t tax_raw) {
        auto &state = groups[group_key];
        if (!state.used) {
            state.used = true;
            used_keys.push_back(group_key);
        }

        ChunkLocalAggregateState local;
        if (hot_loop_config.simplify_exact_derived) {
            UpdateChunkLocalState<true, true>(local, quantity_raw, extendedprice_raw, discount_raw, tax_raw);
        } else {
            UpdateChunkLocalState<true, false>(local, quantity_raw, extendedprice_raw, discount_raw, tax_raw);
        }
        MergeChunkLocalIntoGroup(state, local, hot_loop_config.use_group_int64_partials);
    }

    void ConsumeFlatChunk(duckdb::DataChunk &input) {
        auto shipdates = duckdb::FlatVector::GetData<duckdb::date_t>(input.data[Q1LineitemChunkLayout::SHIPDATE_INDEX]);
        auto quantities = duckdb::FlatVector::GetData<int64_t>(input.data[Q1LineitemChunkLayout::QUANTITY_INDEX]);
        auto extendedprices = duckdb::FlatVector::GetData<int64_t>(input.data[Q1LineitemChunkLayout::EXTENDEDPRICE_INDEX]);
        auto discounts = duckdb::FlatVector::GetData<int64_t>(input.data[Q1LineitemChunkLayout::DISCOUNT_INDEX]);
        auto taxes = duckdb::FlatVector::GetData<int64_t>(input.data[Q1LineitemChunkLayout::TAX_INDEX]);
        auto returnflags = duckdb::FlatVector::GetData<duckdb::string_t>(input.data[Q1LineitemChunkLayout::RETURNFLAG_INDEX]);
        auto linestatuses = duckdb::FlatVector::GetData<duckdb::string_t>(input.data[Q1LineitemChunkLayout::LINESTATUS_INDEX]);

        for (duckdb::idx_t row = 0; row < input.size(); row++) {
            if (!hot_loop_config.trust_native_filter && !(shipdates[row] <= params.cutoff_date)) {
                continue;
            }
            DirectUpdateGroupByKey(PackGroupKeyUnchecked(returnflags[row], linestatuses[row]), quantities[row],
                                   extendedprices[row], discounts[row], taxes[row]);
        }
    }

    void ConsumeDominantDictionaryChunk(duckdb::DataChunk &input, const DominantDictionaryChunkView &view) {
        if (!CanReuseDominantPackedKeyChunkLocalCache(input, view, reusable_dominant_cache)) {
            if (!RebuildReusableDominantPackedKeyChunkLocalCache(input, view, groups, used_keys,
                                                                reusable_dominant_cache)) {
                ConsumeDominantDictionaryChunkFallback(input, view);
                return;
            }
        } else {
            ResetReusableDominantPackedKeyChunkLocalCacheState(reusable_dominant_cache);
        }

        if (reusable_dominant_cache.use_fixed_slots) {
            const auto banks = static_cast<duckdb::idx_t>(hot_loop_config.fixed_slot_banks);
            const auto use_filter = hot_loop_config.trust_native_filter;
            const auto simplify = hot_loop_config.simplify_exact_derived;
            const auto unroll = hot_loop_config.fixed_slot_unroll;

            if (use_filter) {
                if (banks == 8 && unroll == 8) {
                    if (simplify) {
                        ConsumeReusableFixedSlotChunkLocals<true, 8, 8, true>(view, input.size(), params.cutoff_date,
                                                                               reusable_dominant_cache);
                    } else {
                        ConsumeReusableFixedSlotChunkLocals<true, 8, 8, false>(view, input.size(), params.cutoff_date,
                                                                                reusable_dominant_cache);
                    }
                } else if (banks == 4 && unroll >= 4) {
                    if (simplify) {
                        ConsumeReusableFixedSlotChunkLocals<true, 4, 4, true>(view, input.size(), params.cutoff_date,
                                                                               reusable_dominant_cache);
                    } else {
                        ConsumeReusableFixedSlotChunkLocals<true, 4, 4, false>(view, input.size(), params.cutoff_date,
                                                                                reusable_dominant_cache);
                    }
                } else if (banks == 2) {
                    if (simplify) {
                        ConsumeReusableFixedSlotChunkLocals<true, 2, 1, true>(view, input.size(), params.cutoff_date,
                                                                               reusable_dominant_cache);
                    } else {
                        ConsumeReusableFixedSlotChunkLocals<true, 2, 1, false>(view, input.size(), params.cutoff_date,
                                                                                reusable_dominant_cache);
                    }
                } else {
                    if (simplify) {
                        ConsumeReusableFixedSlotChunkLocals<true, 1, 1, true>(view, input.size(), params.cutoff_date,
                                                                               reusable_dominant_cache);
                    } else {
                        ConsumeReusableFixedSlotChunkLocals<true, 1, 1, false>(view, input.size(), params.cutoff_date,
                                                                                reusable_dominant_cache);
                    }
                }
            } else {
                if (banks == 8 && unroll == 8) {
                    if (simplify) {
                        ConsumeReusableFixedSlotChunkLocals<false, 8, 8, true>(view, input.size(), params.cutoff_date,
                                                                                reusable_dominant_cache);
                    } else {
                        ConsumeReusableFixedSlotChunkLocals<false, 8, 8, false>(
                            view, input.size(), params.cutoff_date, reusable_dominant_cache);
                    }
                } else if (banks == 4 && unroll >= 4) {
                    if (simplify) {
                        ConsumeReusableFixedSlotChunkLocals<false, 4, 4, true>(view, input.size(), params.cutoff_date,
                                                                                reusable_dominant_cache);
                    } else {
                        ConsumeReusableFixedSlotChunkLocals<false, 4, 4, false>(
                            view, input.size(), params.cutoff_date, reusable_dominant_cache);
                    }
                } else if (banks == 2) {
                    if (simplify) {
                        ConsumeReusableFixedSlotChunkLocals<false, 2, 1, true>(view, input.size(), params.cutoff_date,
                                                                                reusable_dominant_cache);
                    } else {
                        ConsumeReusableFixedSlotChunkLocals<false, 2, 1, false>(
                            view, input.size(), params.cutoff_date, reusable_dominant_cache);
                    }
                } else {
                    if (simplify) {
                        ConsumeReusableFixedSlotChunkLocals<false, 1, 1, true>(view, input.size(), params.cutoff_date,
                                                                                reusable_dominant_cache);
                    } else {
                        ConsumeReusableFixedSlotChunkLocals<false, 1, 1, false>(
                            view, input.size(), params.cutoff_date, reusable_dominant_cache);
                    }
                }
            }
            MergeReusableFixedSlotChunkLocals(used_keys, reusable_dominant_cache,
                                              hot_loop_config.use_group_int64_partials, banks);
            return;
        }

        if (!reusable_dominant_cache.valid) {
            ConsumeDominantDictionaryChunkFallback(input, view);
            return;
        }

        for (duckdb::idx_t row = 0; row < input.size(); row++) {
            const auto numeric_index = view.numeric_sel->get_index(row);
            if (!hot_loop_config.trust_native_filter && !(view.shipdates[numeric_index] <= params.cutoff_date)) {
                continue;
            }
            const auto returnflag_index = view.returnflag_sel->get_index(row);
            const auto linestatus_index = view.linestatus_sel->get_index(row);
            const auto pair_index =
                returnflag_index * reusable_dominant_cache.linestatus_dictionary_size + linestatus_index;
            auto &local =
                reusable_dominant_cache.slot_states[reusable_dominant_cache.pair_to_slot[pair_index]];
            if (hot_loop_config.simplify_exact_derived) {
                UpdateChunkLocalState<true, true>(local, view.quantities[numeric_index],
                                                  view.extendedprices[numeric_index], view.discounts[numeric_index],
                                                  view.taxes[numeric_index]);
            } else {
                UpdateChunkLocalState<true, false>(local, view.quantities[numeric_index],
                                                   view.extendedprices[numeric_index], view.discounts[numeric_index],
                                                   view.taxes[numeric_index]);
            }
        }
        for (duckdb::idx_t slot = 0; slot < reusable_dominant_cache.slot_states.size(); slot++) {
            const auto &local = reusable_dominant_cache.slot_states[slot];
            if (local.count == 0) {
                continue;
            }
            auto &state = *reusable_dominant_cache.slot_targets[slot];
            if (!state.used) {
                state.used = true;
                used_keys.push_back(reusable_dominant_cache.slot_keys[slot]);
            }
            MergeChunkLocalIntoGroup(state, local, hot_loop_config.use_group_int64_partials);
        }
    }

    void ConsumeDominantDictionaryChunkFallback(duckdb::DataChunk &input, const DominantDictionaryChunkView &view) {
        for (duckdb::idx_t row = 0; row < input.size(); row++) {
            const auto numeric_index = view.numeric_sel->get_index(row);
            if (!hot_loop_config.trust_native_filter && !(view.shipdates[numeric_index] <= params.cutoff_date)) {
                continue;
            }
            const auto returnflag_index = view.returnflag_sel->get_index(row);
            const auto linestatus_index = view.linestatus_sel->get_index(row);
            if ((!view.returnflag_all_valid && !view.returnflag_validity->RowIsValidUnsafe(returnflag_index)) ||
                (!view.linestatus_all_valid && !view.linestatus_validity->RowIsValidUnsafe(linestatus_index))) {
                continue;
            }
            const auto returnflag = view.returnflags[returnflag_index];
            const auto linestatus = view.linestatuses[linestatus_index];
            if (returnflag.GetSize() != 1 || linestatus.GetSize() != 1) {
                continue;
            }
            DirectUpdateGroupByKey(PackGroupKeyUnchecked(returnflag, linestatus),
                                   view.quantities[numeric_index], view.extendedprices[numeric_index],
                                   view.discounts[numeric_index], view.taxes[numeric_index]);
        }
    }

    void ConsumeGenericChunk(duckdb::DataChunk &input) {
        duckdb::UnifiedVectorFormat shipdate_format;
        duckdb::UnifiedVectorFormat quantity_format;
        duckdb::UnifiedVectorFormat extendedprice_format;
        duckdb::UnifiedVectorFormat discount_format;
        duckdb::UnifiedVectorFormat tax_format;
        duckdb::UnifiedVectorFormat returnflag_format;
        duckdb::UnifiedVectorFormat linestatus_format;

        input.data[Q1LineitemChunkLayout::SHIPDATE_INDEX].ToUnifiedFormat(input.size(), shipdate_format);
        input.data[Q1LineitemChunkLayout::QUANTITY_INDEX].ToUnifiedFormat(input.size(), quantity_format);
        input.data[Q1LineitemChunkLayout::EXTENDEDPRICE_INDEX].ToUnifiedFormat(input.size(), extendedprice_format);
        input.data[Q1LineitemChunkLayout::DISCOUNT_INDEX].ToUnifiedFormat(input.size(), discount_format);
        input.data[Q1LineitemChunkLayout::TAX_INDEX].ToUnifiedFormat(input.size(), tax_format);
        input.data[Q1LineitemChunkLayout::RETURNFLAG_INDEX].ToUnifiedFormat(input.size(), returnflag_format);
        input.data[Q1LineitemChunkLayout::LINESTATUS_INDEX].ToUnifiedFormat(input.size(), linestatus_format);

        auto shipdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdate_format);
        auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);
        auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
        auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
        auto taxes = duckdb::UnifiedVectorFormat::GetData<int64_t>(tax_format);
        auto returnflags = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(returnflag_format);
        auto linestatuses = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(linestatus_format);
        const bool shipdate_all_valid = shipdate_format.validity.AllValid();
        const bool quantity_all_valid = quantity_format.validity.AllValid();
        const bool extendedprice_all_valid = extendedprice_format.validity.AllValid();
        const bool discount_all_valid = discount_format.validity.AllValid();
        const bool tax_all_valid = tax_format.validity.AllValid();
        const bool returnflag_all_valid = returnflag_format.validity.AllValid();
        const bool linestatus_all_valid = linestatus_format.validity.AllValid();

        for (duckdb::idx_t row = 0; row < input.size(); row++) {
            const auto shipdate_index = shipdate_format.sel->get_index(row);
            const auto quantity_index = quantity_format.sel->get_index(row);
            const auto extendedprice_index = extendedprice_format.sel->get_index(row);
            const auto discount_index = discount_format.sel->get_index(row);
            const auto tax_index = tax_format.sel->get_index(row);
            const auto returnflag_index = returnflag_format.sel->get_index(row);
            const auto linestatus_index = linestatus_format.sel->get_index(row);

            if ((!shipdate_all_valid && !shipdate_format.validity.RowIsValidUnsafe(shipdate_index)) ||
                (!quantity_all_valid && !quantity_format.validity.RowIsValidUnsafe(quantity_index)) ||
                (!extendedprice_all_valid && !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
                (!discount_all_valid && !discount_format.validity.RowIsValidUnsafe(discount_index)) ||
                (!tax_all_valid && !tax_format.validity.RowIsValidUnsafe(tax_index)) ||
                (!returnflag_all_valid && !returnflag_format.validity.RowIsValidUnsafe(returnflag_index)) ||
                (!linestatus_all_valid && !linestatus_format.validity.RowIsValidUnsafe(linestatus_index))) {
                continue;
            }
            if (!hot_loop_config.trust_native_filter && !(shipdates[shipdate_index] <= params.cutoff_date)) {
                continue;
            }
            DirectUpdateGroupByKey(PackGroupKey(returnflags[returnflag_index], linestatuses[linestatus_index]),
                                   quantities[quantity_index], extendedprices[extendedprice_index],
                                   discounts[discount_index], taxes[tax_index]);
        }
    }

    Q1Parameters params;
    std::array<Q1GroupState, 1u << 16> groups {};
    std::vector<uint16_t> used_keys;
    std::vector<Q1OutputRow> ordered_rows;
    ReusableDominantPackedKeyChunkLocalCache reusable_dominant_cache;
    Q1HotLoopConfig hot_loop_config;
    duckdb::idx_t output_offset = 0;
    bool finalized = false;
};

struct BenchmarkGroupState {
    int64_t sum_qty = 0;
    int64_t sum_base_price = 0;
    int64_t sum_disc_price = 0;
    int64_t sum_charge = 0;
    int64_t sum_discount = 0;
    int64_t count = 0;
};

constexpr duckdb::idx_t kBenchmarkGroupReserve = 16;

struct BenchmarkGroupBuffer {
    std::array<uint16_t, kBenchmarkGroupReserve> used_keys {};
    std::array<BenchmarkGroupState, kBenchmarkGroupReserve> groups {};
    duckdb::idx_t used_count = 0;

    BenchmarkGroupState &Get(uint16_t key) {
        for (duckdb::idx_t idx = 0; idx < used_count; idx++) {
            if (used_keys[idx] == key) {
                return groups[idx];
            }
        }
        if (used_count >= kBenchmarkGroupReserve) {
            throw duckdb::InternalException("Q1 benchmark group buffer exhausted");
        }
        used_keys[used_count] = key;
        groups[used_count] = {};
        return groups[used_count++];
    }

    uint64_t Checksum() const {
        uint64_t checksum = 0;
        for (duckdb::idx_t idx = 0; idx < used_count; idx++) {
            const auto key = used_keys[idx];
            const auto &group = groups[idx];
            checksum ^= static_cast<uint64_t>(key) * 1315423911ULL;
            checksum += static_cast<uint64_t>(group.sum_qty);
            checksum += static_cast<uint64_t>(group.sum_base_price);
            checksum += static_cast<uint64_t>(group.sum_discount);
            checksum += static_cast<uint64_t>(group.sum_disc_price);
            checksum += static_cast<uint64_t>(group.sum_charge);
            checksum += static_cast<uint64_t>(group.count);
        }
        return checksum;
    }
};

struct BenchmarkStageState {
    uint64_t qualifying_rows = 0;
    uint64_t packed_key_sum = 0;
    BenchmarkGroupBuffer groups;
};

template <Q1KernelBenchmarkMode Mode>
inline void BenchmarkConsumeRow(BenchmarkStageState &state,
                                uint16_t key,
                                int64_t quantity_raw,
                                int64_t extendedprice_raw,
                                int64_t discount_raw,
                                int64_t tax_raw) {
    state.qualifying_rows += 1;
    if constexpr (Mode == Q1KernelBenchmarkMode::ResidualOnly) {
        return;
    } else if constexpr (Mode == Q1KernelBenchmarkMode::DecodeAndPackOnly) {
        state.packed_key_sum += static_cast<uint64_t>(key);
        return;
    } else {
        auto &group = state.groups.Get(key);
        group.sum_qty += quantity_raw;
        group.sum_base_price += extendedprice_raw;
        group.sum_discount += discount_raw;
        group.count += 1;
        if constexpr (Mode == Q1KernelBenchmarkMode::SimpleAggregates) {
            return;
        } else {
            const auto one_minus_discount_raw = static_cast<int64_t>(100 - discount_raw);
            group.sum_disc_price += extendedprice_raw * one_minus_discount_raw;
            const auto one_plus_tax_raw = static_cast<int64_t>(100 + tax_raw);
            group.sum_charge += extendedprice_raw * one_minus_discount_raw * one_plus_tax_raw;
        }
    }
}

template <Q1KernelBenchmarkMode Mode>
inline void BenchmarkConsumeRowLocal(BenchmarkStageState &state,
                                     ChunkLocalAggregateBuffer &local_buffer,
                                     uint16_t key,
                                     int64_t quantity_raw,
                                     int64_t extendedprice_raw,
                                     int64_t discount_raw,
                                     int64_t tax_raw) {
    state.qualifying_rows += 1;
    if constexpr (Mode == Q1KernelBenchmarkMode::ResidualOnly) {
        return;
    } else if constexpr (Mode == Q1KernelBenchmarkMode::DecodeAndPackOnly) {
        state.packed_key_sum += static_cast<uint64_t>(key);
        return;
    } else if constexpr (Mode == Q1KernelBenchmarkMode::SimpleAggregates) {
        auto &local = local_buffer.Get(key);
        UpdateChunkLocalState<false, false>(local, quantity_raw, extendedprice_raw, discount_raw, tax_raw);
    } else {
        auto &local = local_buffer.Get(key);
        UpdateChunkLocalState<true, false>(local, quantity_raw, extendedprice_raw, discount_raw, tax_raw);
    }
}

template <Q1KernelBenchmarkMode Mode>
void MergeBenchmarkLocals(BenchmarkStageState &state, const ChunkLocalAggregateBuffer &buffer) {
    if constexpr (Mode == Q1KernelBenchmarkMode::ResidualOnly || Mode == Q1KernelBenchmarkMode::DecodeAndPackOnly) {
        return;
    } else if constexpr (Mode == Q1KernelBenchmarkMode::SimpleAggregates) {
        for (duckdb::idx_t idx = 0; idx < buffer.count; idx++) {
            auto &group = state.groups.Get(buffer.keys[idx]);
            const auto &local = buffer.states[idx];
            group.sum_qty += local.sum_qty;
            group.sum_base_price += local.sum_base_price;
            group.sum_discount += local.sum_discount;
            group.count += local.count;
        }
    } else {
        for (duckdb::idx_t idx = 0; idx < buffer.count; idx++) {
            auto &group = state.groups.Get(buffer.keys[idx]);
            const auto &local = buffer.states[idx];
            group.sum_qty += local.sum_qty;
            group.sum_base_price += local.sum_base_price;
            group.sum_discount += local.sum_discount;
            group.sum_disc_price += local.sum_disc_price;
            group.sum_charge += local.sum_charge;
            group.count += local.count;
        }
    }
}

template <Q1KernelBenchmarkMode Mode>
uint64_t FinalizeBenchmarkStageState(const BenchmarkStageState &state) {
    uint64_t checksum = state.qualifying_rows * 11400714819323198485ull;
    if constexpr (Mode != Q1KernelBenchmarkMode::ResidualOnly) {
        checksum ^= state.packed_key_sum;
    }
    if constexpr (Mode != Q1KernelBenchmarkMode::ResidualOnly && Mode != Q1KernelBenchmarkMode::DecodeAndPackOnly) {
        checksum ^= state.groups.Checksum();
    }
    return checksum;
}

template <bool CollectCounters>
Q1KernelBenchmarkResult BenchmarkResidualOnlyChunk(duckdb::DataChunk &input, const Q1Parameters &parameters) {
    Q1KernelBenchmarkResult result;
    if constexpr (CollectCounters) {
        result.counters.total_chunks = 1;
        result.counters.total_rows = input.size();
    }

    auto &shipdate_vector = input.data[Q1LineitemChunkLayout::SHIPDATE_INDEX];
    uint64_t qualifying_rows = 0;
    if (shipdate_vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
        duckdb::FlatVector::Validity(shipdate_vector).AllValid()) {
        auto shipdates = duckdb::FlatVector::GetData<duckdb::date_t>(shipdate_vector);
        for (duckdb::idx_t row = 0; row < input.size(); row++) {
            qualifying_rows += shipdates[row] <= parameters.cutoff_date;
        }
    } else if (shipdate_vector.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR) {
        auto &sel = duckdb::DictionaryVector::SelVector(shipdate_vector);
        auto &child = duckdb::DictionaryVector::Child(shipdate_vector);
        if (child.GetVectorType() == duckdb::VectorType::FLAT_VECTOR && duckdb::FlatVector::Validity(child).AllValid()) {
            auto shipdates = duckdb::FlatVector::GetData<duckdb::date_t>(child);
            for (duckdb::idx_t row = 0; row < input.size(); row++) {
                qualifying_rows += shipdates[sel.get_index(row)] <= parameters.cutoff_date;
            }
        } else {
            duckdb::UnifiedVectorFormat shipdate_format;
            shipdate_vector.ToUnifiedFormat(input.size(), shipdate_format);
            auto shipdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdate_format);
            const bool all_valid = shipdate_format.validity.AllValid();
            for (duckdb::idx_t row = 0; row < input.size(); row++) {
                const auto index = shipdate_format.sel->get_index(row);
                if (!all_valid && !shipdate_format.validity.RowIsValidUnsafe(index)) {
                    continue;
                }
                qualifying_rows += shipdates[index] <= parameters.cutoff_date;
            }
        }
    } else {
        duckdb::UnifiedVectorFormat shipdate_format;
        shipdate_vector.ToUnifiedFormat(input.size(), shipdate_format);
        auto shipdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdate_format);
        const bool all_valid = shipdate_format.validity.AllValid();
        for (duckdb::idx_t row = 0; row < input.size(); row++) {
            const auto index = shipdate_format.sel->get_index(row);
            if (!all_valid && !shipdate_format.validity.RowIsValidUnsafe(index)) {
                continue;
            }
            qualifying_rows += shipdates[index] <= parameters.cutoff_date;
        }
    }

    if constexpr (CollectCounters) {
        result.counters.qualifying_rows = qualifying_rows;
    }
    result.checksum = qualifying_rows * 11400714819323198485ull;
    return result;
}

template <bool CollectCounters>
Q1KernelBenchmarkResult BenchmarkDominantAdmissionChunk(duckdb::DataChunk &input) {
    Q1KernelBenchmarkResult result;
    if constexpr (CollectCounters) {
        result.counters.total_chunks = 1;
        result.counters.total_rows = input.size();
    }
    DominantDictionaryChunkView view;
    const bool ok = TryGetDominantDictionaryChunkView(input, view);
    if constexpr (CollectCounters) {
        if (ok) {
            result.counters.dominant_fast_path_chunks = 1;
            result.counters.dominant_fast_path_rows = input.size();
        }
    }
    result.checksum = ok ? 1 : 0;
    return result;
}

template <Q1KernelBenchmarkMode Mode, bool CollectCounters>
Q1KernelBenchmarkResult BenchmarkDominantComponentChunk(duckdb::DataChunk &input, const Q1Parameters &parameters) {
    Q1KernelBenchmarkResult result;
    if constexpr (CollectCounters) {
        result.counters.total_chunks = 1;
        result.counters.total_rows = input.size();
    }

    DominantDictionaryChunkView view;
    if (!TryGetDominantDictionaryChunkView(input, view)) {
        return result;
    }
    if constexpr (CollectCounters) {
        result.counters.dominant_fast_path_chunks = 1;
        result.counters.dominant_fast_path_rows = input.size();
    }

    uint64_t qualifying_rows = 0;
    uint64_t sink = 0;
    for (duckdb::idx_t row = 0; row < input.size(); row++) {
        const auto numeric_index = view.numeric_sel->get_index(row);
        const auto returnflag_index = view.returnflag_sel->get_index(row);
        const auto linestatus_index = view.linestatus_sel->get_index(row);
        if (!(view.shipdates[numeric_index] <= parameters.cutoff_date)) {
            continue;
        }
        qualifying_rows += 1;
        if constexpr (Mode == Q1KernelBenchmarkMode::DominantIndicesOnly) {
            sink += numeric_index + returnflag_index + linestatus_index;
            continue;
        }

        if ((!view.returnflag_all_valid && !view.returnflag_validity->RowIsValidUnsafe(returnflag_index)) ||
            (!view.linestatus_all_valid && !view.linestatus_validity->RowIsValidUnsafe(linestatus_index))) {
            continue;
        }
        if constexpr (Mode == Q1KernelBenchmarkMode::DominantIndicesAndValidity) {
            sink += numeric_index + returnflag_index + linestatus_index;
            continue;
        }

        const auto returnflag = view.returnflags[returnflag_index];
        const auto linestatus = view.linestatuses[linestatus_index];
        if (returnflag.GetSize() != 1 || linestatus.GetSize() != 1) {
            continue;
        }
        if constexpr (Mode == Q1KernelBenchmarkMode::DominantIndicesValidityLength) {
            sink += numeric_index + returnflag_index + linestatus_index;
            continue;
        }

        const auto returnflag_char = static_cast<uint8_t>(returnflag.GetDataUnsafe()[0]);
        const auto linestatus_char = static_cast<uint8_t>(linestatus.GetDataUnsafe()[0]);
        if constexpr (Mode == Q1KernelBenchmarkMode::DominantReadChars) {
            sink += returnflag_char + linestatus_char;
            continue;
        }

        if constexpr (Mode == Q1KernelBenchmarkMode::DominantPackKey) {
            sink += static_cast<uint64_t>((static_cast<uint16_t>(returnflag_char) << 8) | linestatus_char);
        }
    }

    if constexpr (CollectCounters) {
        result.counters.qualifying_rows = qualifying_rows;
    }
    result.checksum = qualifying_rows * 11400714819323198485ull ^ sink;
    return result;
}

template <bool UsePackedKeyCache, bool BuildOnly, bool CollectCounters>
Q1KernelBenchmarkResult BenchmarkDominantCachedStringChunk(duckdb::DataChunk &input, const Q1Parameters &parameters) {
    Q1KernelBenchmarkResult result;
    const auto &config = GetQ1HotLoopConfig();
    if constexpr (CollectCounters) {
        result.counters.total_chunks = 1;
        result.counters.total_rows = input.size();
    }

    DominantDictionaryChunkView view;
    if (!TryGetDominantDictionaryChunkView(input, view)) {
        return result;
    }
    if constexpr (CollectCounters) {
        result.counters.dominant_fast_path_chunks = 1;
        result.counters.dominant_fast_path_rows = input.size();
    }

    DominantStringByteCache byte_cache;
    uint64_t checksum = 0;
    if (!BuildDominantStringByteCache(input, view, byte_cache, &checksum)) {
        return result;
    }
    if constexpr (BuildOnly && !UsePackedKeyCache) {
        result.checksum = checksum;
        return result;
    }

    DominantPackedKeyCache packed_key_cache;
    if constexpr (UsePackedKeyCache) {
        BuildDominantPackedKeyCache(byte_cache, packed_key_cache, &checksum);
        if constexpr (BuildOnly) {
            result.checksum = checksum;
            return result;
        }
    }

    uint64_t qualifying_rows = 0;
    uint64_t packed_key_sum = 0;
    for (duckdb::idx_t row = 0; row < input.size(); row++) {
        const auto numeric_index = view.numeric_sel->get_index(row);
        if (!config.trust_native_filter && !(view.shipdates[numeric_index] <= parameters.cutoff_date)) {
            continue;
        }
        const auto returnflag_index = view.returnflag_sel->get_index(row);
        const auto linestatus_index = view.linestatus_sel->get_index(row);
        const uint16_t key = [&]() {
            if constexpr (UsePackedKeyCache) {
                return packed_key_cache
                    .packed_keys[returnflag_index * packed_key_cache.linestatus_dictionary_size + linestatus_index];
            }
            return PackGroupKeyUncheckedBytes(byte_cache.returnflag_chars[returnflag_index],
                                             byte_cache.linestatus_chars[linestatus_index]);
        }();
        qualifying_rows += 1;
        packed_key_sum += key;
    }

    if constexpr (CollectCounters) {
        result.counters.qualifying_rows = qualifying_rows;
    }
    result.checksum = checksum ^ packed_key_sum ^ (qualifying_rows * 11400714819323198485ull);
    return result;
}

template <bool UseFixedSlots, bool CollectCounters>
Q1KernelBenchmarkResult BenchmarkReuseChunkLocalStage(duckdb::DataChunk &input, const Q1Parameters &parameters) {
    Q1KernelBenchmarkResult result;
    const auto &config = GetQ1HotLoopConfig();
    if constexpr (CollectCounters) {
        result.counters.total_chunks = 1;
        result.counters.total_rows = input.size();
    }

    DominantDictionaryChunkView view;
    if (!TryGetDominantDictionaryChunkView(input, view)) {
        return result;
    }
    if constexpr (CollectCounters) {
        result.counters.dominant_fast_path_chunks = 1;
        result.counters.dominant_fast_path_rows = input.size();
    }

    auto groups = duckdb::make_uniq<std::array<Q1GroupState, 1u << 16>>();
    std::vector<uint16_t> used_keys;
    ReusableDominantPackedKeyChunkLocalCache cache;
    if (!RebuildReusableDominantPackedKeyChunkLocalCache(input, view, *groups, used_keys, cache)) {
        return result;
    }

    uint64_t qualifying_rows = 0;
    if constexpr (UseFixedSlots) {
        if (!cache.use_fixed_slots) {
            return result;
        }
        for (duckdb::idx_t row = 0; row < input.size(); row++) {
            const auto numeric_index = view.numeric_sel->get_index(row);
            if (!config.trust_native_filter && !(view.shipdates[numeric_index] <= parameters.cutoff_date)) {
                continue;
            }
            qualifying_rows += 1;
            const auto returnflag_index = view.returnflag_sel->get_index(row);
            const auto linestatus_index = view.linestatus_sel->get_index(row);
            const auto pair_index = returnflag_index * cache.linestatus_dictionary_size + linestatus_index;
            const auto slot = cache.fixed_pair_to_slot[pair_index];
            auto &local = cache.fixed_slot_bank_states[0][slot];
            UpdateChunkLocalState<true, false>(local, view.quantities[numeric_index],
                                               view.extendedprices[numeric_index], view.discounts[numeric_index],
                                               view.taxes[numeric_index]);
        }
        MergeReusableFixedSlotChunkLocals(used_keys, cache, false, 1);
    } else {
        for (duckdb::idx_t row = 0; row < input.size(); row++) {
            const auto numeric_index = view.numeric_sel->get_index(row);
            if (!config.trust_native_filter && !(view.shipdates[numeric_index] <= parameters.cutoff_date)) {
                continue;
            }
            qualifying_rows += 1;
            const auto returnflag_index = view.returnflag_sel->get_index(row);
            const auto linestatus_index = view.linestatus_sel->get_index(row);
            const auto pair_index = returnflag_index * cache.linestatus_dictionary_size + linestatus_index;
            auto &local = cache.slot_states[cache.pair_to_slot[pair_index]];
            UpdateChunkLocalState<true, false>(local, view.quantities[numeric_index],
                                               view.extendedprices[numeric_index], view.discounts[numeric_index],
                                               view.taxes[numeric_index]);
        }
        for (duckdb::idx_t slot = 0; slot < cache.slot_states.size(); slot++) {
            const auto &local = cache.slot_states[slot];
            if (local.count == 0) {
                continue;
            }
            auto &state = *cache.slot_targets[slot];
            if (!state.used) {
                state.used = true;
                used_keys.push_back(cache.slot_keys[slot]);
            }
            AddValue(state.sum_qty, local.sum_qty);
            AddValue(state.sum_base_price, local.sum_base_price);
            AddValue(state.sum_discount, local.sum_discount);
            AddValue(state.sum_disc_price, local.sum_disc_price);
            AddValue(state.sum_charge, local.sum_charge);
            state.count += static_cast<uint64_t>(local.count);
        }
    }

    uint64_t checksum = qualifying_rows * 11400714819323198485ull;
    for (const auto key : used_keys) {
        const auto &state = (*groups)[key];
        checksum ^= static_cast<uint64_t>(key) * 1315423911ULL;
        checksum += static_cast<uint64_t>(state.count);
    }
    if constexpr (CollectCounters) {
        result.counters.qualifying_rows = qualifying_rows;
    }
    result.checksum = checksum;
    return result;
}

template <Q1KernelBenchmarkMode Mode, bool CollectCounters>
void BenchmarkFlatChunk(duckdb::DataChunk &input,
                        const Q1Parameters &parameters,
                        Q1KernelBenchmarkResult &result,
                        BenchmarkStageState &state,
                        bool use_chunk_local_partials) {
    const auto &config = GetQ1HotLoopConfig();
    auto shipdates = duckdb::FlatVector::GetData<duckdb::date_t>(input.data[Q1LineitemChunkLayout::SHIPDATE_INDEX]);
    auto quantities = duckdb::FlatVector::GetData<int64_t>(input.data[Q1LineitemChunkLayout::QUANTITY_INDEX]);
    auto extendedprices = duckdb::FlatVector::GetData<int64_t>(input.data[Q1LineitemChunkLayout::EXTENDEDPRICE_INDEX]);
    auto discounts = duckdb::FlatVector::GetData<int64_t>(input.data[Q1LineitemChunkLayout::DISCOUNT_INDEX]);
    auto taxes = duckdb::FlatVector::GetData<int64_t>(input.data[Q1LineitemChunkLayout::TAX_INDEX]);
    auto returnflags = duckdb::FlatVector::GetData<duckdb::string_t>(input.data[Q1LineitemChunkLayout::RETURNFLAG_INDEX]);
    auto linestatuses = duckdb::FlatVector::GetData<duckdb::string_t>(input.data[Q1LineitemChunkLayout::LINESTATUS_INDEX]);

    if constexpr (CollectCounters) {
        result.counters.dominant_fast_path_chunks += 1;
        result.counters.dominant_fast_path_rows += input.size();
    }
    if (use_chunk_local_partials) {
        ChunkLocalAggregateBuffer local_buffer;
        for (duckdb::idx_t row = 0; row < input.size(); row++) {
            if (!config.trust_native_filter && !(shipdates[row] <= parameters.cutoff_date)) {
                continue;
            }
            BenchmarkConsumeRowLocal<Mode>(state, local_buffer,
                                           PackGroupKeyUnchecked(returnflags[row], linestatuses[row]), quantities[row],
                                           extendedprices[row], discounts[row], taxes[row]);
        }
        MergeBenchmarkLocals<Mode>(state, local_buffer);
    } else {
        for (duckdb::idx_t row = 0; row < input.size(); row++) {
            if (!config.trust_native_filter && !(shipdates[row] <= parameters.cutoff_date)) {
                continue;
            }
            BenchmarkConsumeRow<Mode>(state, PackGroupKeyUnchecked(returnflags[row], linestatuses[row]), quantities[row],
                                      extendedprices[row], discounts[row], taxes[row]);
        }
    }
}

template <Q1KernelBenchmarkMode Mode, bool CollectCounters>
void BenchmarkDominantDictionaryChunk(duckdb::DataChunk &input,
                                      const Q1Parameters &parameters,
                                      const DominantDictionaryChunkView &view,
                                      Q1KernelBenchmarkResult &result,
                                      BenchmarkStageState &state,
                                      bool use_chunk_local_partials) {
    const auto &config = GetQ1HotLoopConfig();
    if constexpr (CollectCounters) {
        result.counters.dominant_fast_path_chunks += 1;
        result.counters.dominant_fast_path_rows += input.size();
    }
    DominantStringByteCache byte_cache;
    const bool use_byte_cache = BuildDominantStringByteCache(input, view, byte_cache);
    if (use_chunk_local_partials) {
        ChunkLocalAggregateBuffer local_buffer;
        for (duckdb::idx_t row = 0; row < input.size(); row++) {
            const auto numeric_index = view.numeric_sel->get_index(row);
            if (!config.trust_native_filter && !(view.shipdates[numeric_index] <= parameters.cutoff_date)) {
                continue;
            }
            const auto returnflag_index = view.returnflag_sel->get_index(row);
            const auto linestatus_index = view.linestatus_sel->get_index(row);
            const auto key =
                use_byte_cache
                    ? PackGroupKeyUncheckedBytes(byte_cache.returnflag_chars[returnflag_index],
                                                 byte_cache.linestatus_chars[linestatus_index])
                    : PackGroupKeyUnchecked(view.returnflags[returnflag_index], view.linestatuses[linestatus_index]);
            BenchmarkConsumeRowLocal<Mode>(state, local_buffer, key, view.quantities[numeric_index],
                                           view.extendedprices[numeric_index], view.discounts[numeric_index],
                                           view.taxes[numeric_index]);
        }
        MergeBenchmarkLocals<Mode>(state, local_buffer);
    } else {
        for (duckdb::idx_t row = 0; row < input.size(); row++) {
            const auto numeric_index = view.numeric_sel->get_index(row);
            if (!config.trust_native_filter && !(view.shipdates[numeric_index] <= parameters.cutoff_date)) {
                continue;
            }
            const auto returnflag_index = view.returnflag_sel->get_index(row);
            const auto linestatus_index = view.linestatus_sel->get_index(row);
            const auto key =
                use_byte_cache
                    ? PackGroupKeyUncheckedBytes(byte_cache.returnflag_chars[returnflag_index],
                                                 byte_cache.linestatus_chars[linestatus_index])
                    : PackGroupKeyUnchecked(view.returnflags[returnflag_index], view.linestatuses[linestatus_index]);
            BenchmarkConsumeRow<Mode>(state, key, view.quantities[numeric_index], view.extendedprices[numeric_index],
                                      view.discounts[numeric_index], view.taxes[numeric_index]);
        }
    }
}

template <Q1KernelBenchmarkMode Mode, bool CollectCounters>
void BenchmarkGenericChunk(duckdb::DataChunk &input,
                           const Q1Parameters &parameters,
                           Q1KernelBenchmarkResult &result,
                           BenchmarkStageState &state,
                           bool use_chunk_local_partials) {
    const auto &config = GetQ1HotLoopConfig();
    duckdb::UnifiedVectorFormat shipdate_format;
    duckdb::UnifiedVectorFormat quantity_format;
    duckdb::UnifiedVectorFormat extendedprice_format;
    duckdb::UnifiedVectorFormat discount_format;
    duckdb::UnifiedVectorFormat tax_format;
    duckdb::UnifiedVectorFormat returnflag_format;
    duckdb::UnifiedVectorFormat linestatus_format;

    input.data[Q1LineitemChunkLayout::SHIPDATE_INDEX].ToUnifiedFormat(input.size(), shipdate_format);
    input.data[Q1LineitemChunkLayout::QUANTITY_INDEX].ToUnifiedFormat(input.size(), quantity_format);
    input.data[Q1LineitemChunkLayout::EXTENDEDPRICE_INDEX].ToUnifiedFormat(input.size(), extendedprice_format);
    input.data[Q1LineitemChunkLayout::DISCOUNT_INDEX].ToUnifiedFormat(input.size(), discount_format);
    input.data[Q1LineitemChunkLayout::TAX_INDEX].ToUnifiedFormat(input.size(), tax_format);
    input.data[Q1LineitemChunkLayout::RETURNFLAG_INDEX].ToUnifiedFormat(input.size(), returnflag_format);
    input.data[Q1LineitemChunkLayout::LINESTATUS_INDEX].ToUnifiedFormat(input.size(), linestatus_format);

    auto shipdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdate_format);
    auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);
    auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
    auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
    auto taxes = duckdb::UnifiedVectorFormat::GetData<int64_t>(tax_format);
    auto returnflags = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(returnflag_format);
    auto linestatuses = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(linestatus_format);
    const bool shipdate_all_valid = shipdate_format.validity.AllValid();
    const bool quantity_all_valid = quantity_format.validity.AllValid();
    const bool extendedprice_all_valid = extendedprice_format.validity.AllValid();
    const bool discount_all_valid = discount_format.validity.AllValid();
    const bool tax_all_valid = tax_format.validity.AllValid();
    const bool returnflag_all_valid = returnflag_format.validity.AllValid();
    const bool linestatus_all_valid = linestatus_format.validity.AllValid();

    ChunkLocalAggregateBuffer local_buffer;
    for (duckdb::idx_t row = 0; row < input.size(); row++) {
        const auto shipdate_index = shipdate_format.sel->get_index(row);
        const auto quantity_index = quantity_format.sel->get_index(row);
        const auto extendedprice_index = extendedprice_format.sel->get_index(row);
        const auto discount_index = discount_format.sel->get_index(row);
        const auto tax_index = tax_format.sel->get_index(row);
        const auto returnflag_index = returnflag_format.sel->get_index(row);
        const auto linestatus_index = linestatus_format.sel->get_index(row);

        if ((!shipdate_all_valid && !shipdate_format.validity.RowIsValidUnsafe(shipdate_index)) ||
            (!quantity_all_valid && !quantity_format.validity.RowIsValidUnsafe(quantity_index)) ||
            (!extendedprice_all_valid && !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
            (!discount_all_valid && !discount_format.validity.RowIsValidUnsafe(discount_index)) ||
            (!tax_all_valid && !tax_format.validity.RowIsValidUnsafe(tax_index)) ||
            (!returnflag_all_valid && !returnflag_format.validity.RowIsValidUnsafe(returnflag_index)) ||
            (!linestatus_all_valid && !linestatus_format.validity.RowIsValidUnsafe(linestatus_index))) {
            continue;
        }
        if (!config.trust_native_filter && !(shipdates[shipdate_index] <= parameters.cutoff_date)) {
            continue;
        }
        const auto key = PackGroupKey(returnflags[returnflag_index], linestatuses[linestatus_index]);
        if (use_chunk_local_partials) {
            BenchmarkConsumeRowLocal<Mode>(state, local_buffer, key, quantities[quantity_index],
                                           extendedprices[extendedprice_index], discounts[discount_index],
                                           taxes[tax_index]);
        } else {
            BenchmarkConsumeRow<Mode>(state, key, quantities[quantity_index],
                                      extendedprices[extendedprice_index], discounts[discount_index], taxes[tax_index]);
        }
    }
    if (use_chunk_local_partials) {
        MergeBenchmarkLocals<Mode>(state, local_buffer);
    }
}

template <Q1KernelBenchmarkMode Mode, bool CollectCounters>
Q1KernelBenchmarkResult BenchmarkQ1ChunkImpl(duckdb::DataChunk &input,
                                             const Q1Parameters &parameters,
                                             bool use_chunk_local_partials) {
    Q1KernelBenchmarkResult result;
    if constexpr (CollectCounters) {
        result.counters.total_chunks = 1;
        result.counters.total_rows = input.size();
    }
    BenchmarkStageState state;

    if (Q1ChunkCanUseFlatFastPath(input)) {
        BenchmarkFlatChunk<Mode, CollectCounters>(input, parameters, result, state, use_chunk_local_partials);
        if constexpr (CollectCounters) {
            result.counters.qualifying_rows = state.qualifying_rows;
        }
        result.checksum = FinalizeBenchmarkStageState<Mode>(state);
        return result;
    }

    DominantDictionaryChunkView dominant_view;
    if (TryGetDominantDictionaryChunkView(input, dominant_view)) {
        BenchmarkDominantDictionaryChunk<Mode, CollectCounters>(input, parameters, dominant_view, result, state,
                                                                use_chunk_local_partials);
        if constexpr (CollectCounters) {
            result.counters.qualifying_rows = state.qualifying_rows;
        }
        result.checksum = FinalizeBenchmarkStageState<Mode>(state);
        return result;
    }

    BenchmarkGenericChunk<Mode, CollectCounters>(input, parameters, result, state, use_chunk_local_partials);
    if constexpr (CollectCounters) {
        result.counters.qualifying_rows = state.qualifying_rows;
    }
    result.checksum = FinalizeBenchmarkStageState<Mode>(state);
    return result;
}

} // namespace

bool Q1ChunkCanUseFlatFastPath(duckdb::DataChunk &input) {
    auto &shipdate_vector = input.data[Q1LineitemChunkLayout::SHIPDATE_INDEX];
    auto &quantity_vector = input.data[Q1LineitemChunkLayout::QUANTITY_INDEX];
    auto &extendedprice_vector = input.data[Q1LineitemChunkLayout::EXTENDEDPRICE_INDEX];
    auto &discount_vector = input.data[Q1LineitemChunkLayout::DISCOUNT_INDEX];
    auto &tax_vector = input.data[Q1LineitemChunkLayout::TAX_INDEX];
    auto &returnflag_vector = input.data[Q1LineitemChunkLayout::RETURNFLAG_INDEX];
    auto &linestatus_vector = input.data[Q1LineitemChunkLayout::LINESTATUS_INDEX];

    const bool flat_fast_path =
        shipdate_vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
        quantity_vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
        extendedprice_vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
        discount_vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
        tax_vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
        returnflag_vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
        linestatus_vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
        duckdb::FlatVector::Validity(shipdate_vector).AllValid() &&
        duckdb::FlatVector::Validity(quantity_vector).AllValid() &&
        duckdb::FlatVector::Validity(extendedprice_vector).AllValid() &&
        duckdb::FlatVector::Validity(discount_vector).AllValid() &&
        duckdb::FlatVector::Validity(tax_vector).AllValid() &&
        duckdb::FlatVector::Validity(returnflag_vector).AllValid() &&
        duckdb::FlatVector::Validity(linestatus_vector).AllValid();

    if (!flat_fast_path) {
        return false;
    }

    auto returnflags = duckdb::FlatVector::GetData<duckdb::string_t>(returnflag_vector);
    auto linestatuses = duckdb::FlatVector::GetData<duckdb::string_t>(linestatus_vector);
    return FlatChunkFlagsAreSingleChar(returnflags, linestatuses, input.size());
}

Q1ScanDiagnostics InspectQ1Chunk(duckdb::DataChunk &input, const Q1Parameters &parameters) {
    if (Q1ChunkCanUseFlatFastPath(input)) {
        return InspectFlatChunk(input, parameters);
    }
    DominantDictionaryChunkView dominant_view;
    if (TryGetDominantDictionaryChunkView(input, dominant_view)) {
        return InspectDominantDictionaryChunk(input, parameters);
    }
    return InspectGenericChunk(input, parameters);
}

Q1KernelBenchmarkResult BenchmarkQ1KernelChunk(duckdb::DataChunk &input,
                                               const Q1Parameters &parameters,
                                               Q1KernelBenchmarkMode mode,
                                               bool use_chunk_local_partials,
                                               bool collect_counters) {
    if (mode == Q1KernelBenchmarkMode::ResidualOnly) {
        return collect_counters ? BenchmarkResidualOnlyChunk<true>(input, parameters)
                                : BenchmarkResidualOnlyChunk<false>(input, parameters);
    }
    if (mode == Q1KernelBenchmarkMode::DominantAdmissionOnly) {
        return collect_counters ? BenchmarkDominantAdmissionChunk<true>(input)
                                : BenchmarkDominantAdmissionChunk<false>(input);
    }
    if (mode == Q1KernelBenchmarkMode::DominantIndicesOnly) {
        return collect_counters ? BenchmarkDominantComponentChunk<Q1KernelBenchmarkMode::DominantIndicesOnly, true>(
                                      input, parameters)
                                : BenchmarkDominantComponentChunk<Q1KernelBenchmarkMode::DominantIndicesOnly, false>(
                                      input, parameters);
    }
    if (mode == Q1KernelBenchmarkMode::DominantIndicesAndValidity) {
        return collect_counters
                   ? BenchmarkDominantComponentChunk<Q1KernelBenchmarkMode::DominantIndicesAndValidity, true>(
                         input, parameters)
                   : BenchmarkDominantComponentChunk<Q1KernelBenchmarkMode::DominantIndicesAndValidity, false>(
                         input, parameters);
    }
    if (mode == Q1KernelBenchmarkMode::DominantIndicesValidityLength) {
        return collect_counters
                   ? BenchmarkDominantComponentChunk<Q1KernelBenchmarkMode::DominantIndicesValidityLength, true>(
                         input, parameters)
                   : BenchmarkDominantComponentChunk<Q1KernelBenchmarkMode::DominantIndicesValidityLength, false>(
                         input, parameters);
    }
    if (mode == Q1KernelBenchmarkMode::DominantReadChars) {
        return collect_counters
                   ? BenchmarkDominantComponentChunk<Q1KernelBenchmarkMode::DominantReadChars, true>(input,
                                                                                                      parameters)
                   : BenchmarkDominantComponentChunk<Q1KernelBenchmarkMode::DominantReadChars, false>(input,
                                                                                                       parameters);
    }
    if (mode == Q1KernelBenchmarkMode::DominantPackKey) {
        return collect_counters
                   ? BenchmarkDominantComponentChunk<Q1KernelBenchmarkMode::DominantPackKey, true>(input, parameters)
                   : BenchmarkDominantComponentChunk<Q1KernelBenchmarkMode::DominantPackKey, false>(input, parameters);
    }
    if (mode == Q1KernelBenchmarkMode::DominantByteCacheBuildOnly) {
        return collect_counters ? BenchmarkDominantCachedStringChunk<false, true, true>(input, parameters)
                                : BenchmarkDominantCachedStringChunk<false, true, false>(input, parameters);
    }
    if (mode == Q1KernelBenchmarkMode::DominantByteCache) {
        return collect_counters ? BenchmarkDominantCachedStringChunk<false, false, true>(input, parameters)
                                : BenchmarkDominantCachedStringChunk<false, false, false>(input, parameters);
    }
    if (mode == Q1KernelBenchmarkMode::DominantPackedKeyCacheBuildOnly) {
        return collect_counters ? BenchmarkDominantCachedStringChunk<true, true, true>(input, parameters)
                                : BenchmarkDominantCachedStringChunk<true, true, false>(input, parameters);
    }
    if (mode == Q1KernelBenchmarkMode::DominantPackedKeyCache) {
        return collect_counters ? BenchmarkDominantCachedStringChunk<true, false, true>(input, parameters)
                                : BenchmarkDominantCachedStringChunk<true, false, false>(input, parameters);
    }
    if (mode == Q1KernelBenchmarkMode::ReuseChunkLocalsVector) {
        return collect_counters ? BenchmarkReuseChunkLocalStage<false, true>(input, parameters)
                                : BenchmarkReuseChunkLocalStage<false, false>(input, parameters);
    }
    if (mode == Q1KernelBenchmarkMode::ReuseChunkLocalsFixed) {
        return collect_counters ? BenchmarkReuseChunkLocalStage<true, true>(input, parameters)
                                : BenchmarkReuseChunkLocalStage<true, false>(input, parameters);
    }
    if (collect_counters) {
        switch (mode) {
        case Q1KernelBenchmarkMode::ResidualOnly:
        case Q1KernelBenchmarkMode::DominantAdmissionOnly:
        case Q1KernelBenchmarkMode::DominantIndicesOnly:
        case Q1KernelBenchmarkMode::DominantIndicesAndValidity:
        case Q1KernelBenchmarkMode::DominantIndicesValidityLength:
        case Q1KernelBenchmarkMode::DominantReadChars:
        case Q1KernelBenchmarkMode::DominantPackKey:
        case Q1KernelBenchmarkMode::DominantByteCacheBuildOnly:
        case Q1KernelBenchmarkMode::DominantByteCache:
        case Q1KernelBenchmarkMode::DominantPackedKeyCacheBuildOnly:
        case Q1KernelBenchmarkMode::DominantPackedKeyCache:
        case Q1KernelBenchmarkMode::ReuseChunkLocalsVector:
        case Q1KernelBenchmarkMode::ReuseChunkLocalsFixed:
            break;
        case Q1KernelBenchmarkMode::DecodeAndPackOnly:
            return BenchmarkQ1ChunkImpl<Q1KernelBenchmarkMode::DecodeAndPackOnly, true>(input, parameters,
                                                                                         use_chunk_local_partials);
        case Q1KernelBenchmarkMode::SimpleAggregates:
            return BenchmarkQ1ChunkImpl<Q1KernelBenchmarkMode::SimpleAggregates, true>(input, parameters,
                                                                                        use_chunk_local_partials);
        case Q1KernelBenchmarkMode::DerivedAggregates:
            return BenchmarkQ1ChunkImpl<Q1KernelBenchmarkMode::DerivedAggregates, true>(input, parameters,
                                                                                         use_chunk_local_partials);
        case Q1KernelBenchmarkMode::Full:
            return BenchmarkQ1ChunkImpl<Q1KernelBenchmarkMode::Full, true>(input, parameters, use_chunk_local_partials);
        }
    } else {
        switch (mode) {
        case Q1KernelBenchmarkMode::ResidualOnly:
        case Q1KernelBenchmarkMode::DominantAdmissionOnly:
        case Q1KernelBenchmarkMode::DominantIndicesOnly:
        case Q1KernelBenchmarkMode::DominantIndicesAndValidity:
        case Q1KernelBenchmarkMode::DominantIndicesValidityLength:
        case Q1KernelBenchmarkMode::DominantReadChars:
        case Q1KernelBenchmarkMode::DominantPackKey:
        case Q1KernelBenchmarkMode::DominantByteCacheBuildOnly:
        case Q1KernelBenchmarkMode::DominantByteCache:
        case Q1KernelBenchmarkMode::DominantPackedKeyCacheBuildOnly:
        case Q1KernelBenchmarkMode::DominantPackedKeyCache:
        case Q1KernelBenchmarkMode::ReuseChunkLocalsVector:
        case Q1KernelBenchmarkMode::ReuseChunkLocalsFixed:
            break;
        case Q1KernelBenchmarkMode::DecodeAndPackOnly:
            return BenchmarkQ1ChunkImpl<Q1KernelBenchmarkMode::DecodeAndPackOnly, false>(input, parameters,
                                                                                          use_chunk_local_partials);
        case Q1KernelBenchmarkMode::SimpleAggregates:
            return BenchmarkQ1ChunkImpl<Q1KernelBenchmarkMode::SimpleAggregates, false>(input, parameters,
                                                                                         use_chunk_local_partials);
        case Q1KernelBenchmarkMode::DerivedAggregates:
            return BenchmarkQ1ChunkImpl<Q1KernelBenchmarkMode::DerivedAggregates, false>(input, parameters,
                                                                                          use_chunk_local_partials);
        case Q1KernelBenchmarkMode::Full:
            return BenchmarkQ1ChunkImpl<Q1KernelBenchmarkMode::Full, false>(input, parameters,
                                                                            use_chunk_local_partials);
            break;
        }
    }
    throw duckdb::InternalException("unhandled Q1 benchmark mode");
}

duckdb::unique_ptr<KernelInstance> CreateQ1Kernel(const Q1Parameters &params) {
    return duckdb::make_uniq<Q1Kernel>(params);
}

} // namespace accelerator_codegen
