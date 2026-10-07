// Shared building blocks for the stateless TPC-H Q8 accelerators (single-threaded and parallel).
// All state is per query call; nothing is cached across calls.
//
//  * KeyBitmap   : bitmap set over dense integer keys (qualifying parts, customers in the region) with an
//                  AVX-512 gather-based batch probe.
//  * OrderSlotMap: orderkey -> year slot, 2 bits per order (0 = not qualifying, 1/2 = 1995/1996).  One
//                  contiguous array, ~1.5 MB for SF1, instead of a bitmap plus a 6 MB byte array.
//  * SupplierNations: dense suppkey -> nationkey table.
//  * ProcessLineitemChunk: part probe over all rows (bitmap is L1-resident), order probe over the ~20% that
//    pass, supplier/volume accumulation over the ~6% survivors.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "accelerator_codegen/runtime/simd.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tpch/q8/duckdb/base/query.hpp"

namespace accelerator_codegen {
namespace q8 {

using acc_t = __int128_t;

inline constexpr int kYearSlots = 2;
inline constexpr int64_t kMinYear = 1995;
inline constexpr int32_t kAbsentNation = -1;
inline constexpr int32_t kMaxNationSlots = 25;
//! Keys at or above this live in overflow hash containers instead of the dense arrays.
inline constexpr int64_t kDenseKeyLimit = int64_t {1} << 28;

struct VolumeState {
	acc_t nation_volume = 0;
	acc_t total_volume = 0;
};

using YearVolumes = std::array<VolumeState, kYearSlots>;

inline duckdb::hugeint_t ToHugeint(acc_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}

inline double ComputeMarketShare(const VolumeState &state) {
	const auto nation_decimal = duckdb::Value::DECIMAL(ToHugeint(state.nation_volume), 38, 4);
	const auto total_decimal = duckdb::Value::DECIMAL(ToHugeint(state.total_volume), 38, 4);
	const auto nation_double = nation_decimal.DefaultCastAs(duckdb::LogicalType::DOUBLE).GetValue<double>();
	const auto total_double = total_decimal.DefaultCastAs(duckdb::LogicalType::DOUBLE).GetValue<double>();
	if (total_double == 0.0) {
		return 0.0;
	}
	return nation_double / total_double;
}

inline bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

inline int PopCount64(uint64_t value) {
	return __builtin_popcountll(value);
}

class KeyBitmap {
public:
	void Reserve(int64_t max_key) {
		if (max_key >= 0 && max_key < kDenseKeyLimit) {
			const size_t words = (static_cast<size_t>(max_key) >> 6) + 1;
			if (words > words_.size()) {
				words_.resize(words, 0);
			}
		}
	}

	void Set(int64_t key) {
		if (key < 0) {
			return;
		}
		if (key >= kDenseKeyLimit) {
			overflow_.insert(key);
			return;
		}
		Reserve(key);
		words_[static_cast<size_t>(key) >> 6] |= uint64_t {1} << (key & 63);
	}

	bool Contains(int64_t key) const {
		if (key < 0) {
			return false;
		}
		const auto bit_index = static_cast<uint64_t>(key);
		if ((bit_index >> 6) < words_.size()) {
			return (words_[bit_index >> 6] >> (bit_index & 63)) & 1;
		}
		return !overflow_.empty() && overflow_.count(key) != 0;
	}

	//! rows[0..m) <- ascending indexes i < n with keys[i] in the set.  rows needs n + simd::kSelSlack entries.
	size_t Probe(const int64_t *ACCEL_RESTRICT keys, size_t n, uint32_t *ACCEL_RESTRICT rows) const {
		size_t m = 0;
		size_t i = 0;
		if (!overflow_.empty()) {
			for (; i < n; i++) {
				rows[m] = static_cast<uint32_t>(i);
				m += Contains(keys[i]) ? 1 : 0;
			}
			return m;
		}
		const uint64_t limit = static_cast<uint64_t>(words_.size()) << 6;
#if defined(ACCEL_SIMD_AVX512) && !defined(ACCEL_FORCE_SCALAR_PROBE)
		// 16 keys per step: range check on the 64-bit keys, then a 32-bit gather of the bitmap dwords.
		const __m512i vlimit = _mm512_set1_epi64(static_cast<long long>(limit));
		const __m512i low5 = _mm512_set1_epi32(31);
		const __m512i one = _mm512_set1_epi32(1);
		const __m512i iota = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
		const auto *base = reinterpret_cast<const int *>(words_.data());
		for (; i + 16 <= n; i += 16) {
			const __m512i key_lo = _mm512_loadu_si512(keys + i);
			const __m512i key_hi = _mm512_loadu_si512(keys + i + 8);
			const __mmask16 in_range = static_cast<__mmask16>(_mm512_cmplt_epu64_mask(key_lo, vlimit) |
			                                                  (_mm512_cmplt_epu64_mask(key_hi, vlimit) << 8));
			const __m512i key = _mm512_inserti64x4(_mm512_castsi256_si512(_mm512_cvtepi64_epi32(key_lo)),
			                                       _mm512_cvtepi64_epi32(key_hi), 1);
			const __m512i words =
			    _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), in_range, _mm512_srli_epi32(key, 5), base, 4);
			const __m512i bits = _mm512_srlv_epi32(words, _mm512_and_si512(key, low5));
			const __mmask16 hit = in_range & _mm512_test_epi32_mask(bits, one);
			_mm512_storeu_si512(rows + m, _mm512_maskz_compress_epi32(
			                                  hit, _mm512_add_epi32(iota, _mm512_set1_epi32(static_cast<int>(i)))));
			m += static_cast<size_t>(PopCount64(hit));
		}
#endif
		for (; i < n; i++) {
			const auto key = static_cast<uint64_t>(keys[i]);
			const bool in_range = key < limit;
			const uint64_t word = in_range ? words_[key >> 6] : 0;
			rows[m] = static_cast<uint32_t>(i);
			m += static_cast<size_t>(in_range & ((word >> (key & 63)) & 1));
		}
		return m;
	}

private:
	std::vector<uint64_t> words_;
	std::unordered_set<int64_t> overflow_;
};

class OrderSlotMap {
public:
	void Reserve(int64_t max_key) {
		if (max_key >= 0 && max_key < kDenseKeyLimit) {
			const size_t words = (static_cast<size_t>(max_key) >> 4) + 1;
			if (words > words_.size()) {
				words_.resize(words, 0);
			}
		}
	}

	//! slot in {0, 1}; stored as slot + 1 in 2 bits per order key.
	void Set(int64_t key, int slot) {
		if (key < 0) {
			return;
		}
		if (key >= kDenseKeyLimit) {
			overflow_[key] = static_cast<uint8_t>(slot + 1);
			return;
		}
		Reserve(key);
		auto &word = words_[static_cast<size_t>(key) >> 4];
		const unsigned shift = static_cast<unsigned>(key & 15) << 1;
		word = (word & ~(uint32_t {3} << shift)) | (static_cast<uint32_t>(slot + 1) << shift);
	}

	//! 0 = not qualifying, 1 = slot 0, 2 = slot 1.
	unsigned Get(int64_t key) const {
		if (key < 0) {
			return 0;
		}
		const auto index = static_cast<uint64_t>(key);
		if ((index >> 4) < words_.size()) {
			return (words_[index >> 4] >> ((index & 15) << 1)) & 3;
		}
		if (overflow_.empty()) {
			return 0;
		}
		const auto it = overflow_.find(key);
		return it == overflow_.end() ? 0u : it->second;
	}

	//! For keys[i] that qualify: rows[m] = i, slots[m] = slot + 1.  Outputs need n + simd::kSelSlack entries.
	size_t Probe(const int64_t *ACCEL_RESTRICT keys, size_t n, uint32_t *ACCEL_RESTRICT rows,
	             int32_t *ACCEL_RESTRICT slots) const {
		size_t m = 0;
		size_t i = 0;
		if (!overflow_.empty()) {
			for (; i < n; i++) {
				const auto slot = Get(keys[i]);
				rows[m] = static_cast<uint32_t>(i);
				slots[m] = static_cast<int32_t>(slot);
				m += slot != 0 ? 1 : 0;
			}
			return m;
		}
		const uint64_t limit = static_cast<uint64_t>(words_.size()) << 4;
#if defined(ACCEL_SIMD_AVX512) && !defined(ACCEL_FORCE_SCALAR_PROBE)
		const __m512i vlimit = _mm512_set1_epi64(static_cast<long long>(limit));
		const __m512i low4 = _mm512_set1_epi32(15);
		const __m512i three = _mm512_set1_epi32(3);
		const __m512i iota = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
		const auto *base = reinterpret_cast<const int *>(words_.data());
		for (; i + 16 <= n; i += 16) {
			const __m512i key_lo = _mm512_loadu_si512(keys + i);
			const __m512i key_hi = _mm512_loadu_si512(keys + i + 8);
			const __mmask16 in_range = static_cast<__mmask16>(_mm512_cmplt_epu64_mask(key_lo, vlimit) |
			                                                  (_mm512_cmplt_epu64_mask(key_hi, vlimit) << 8));
			const __m512i key = _mm512_inserti64x4(_mm512_castsi256_si512(_mm512_cvtepi64_epi32(key_lo)),
			                                       _mm512_cvtepi64_epi32(key_hi), 1);
			const __m512i words =
			    _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), in_range, _mm512_srli_epi32(key, 4), base, 4);
			const __m512i shift = _mm512_slli_epi32(_mm512_and_si512(key, low4), 1);
			const __m512i value = _mm512_and_si512(_mm512_srlv_epi32(words, shift), three);
			const __mmask16 hit = in_range & _mm512_test_epi32_mask(value, value);
			_mm512_storeu_si512(rows + m, _mm512_maskz_compress_epi32(
			                                  hit, _mm512_add_epi32(iota, _mm512_set1_epi32(static_cast<int>(i)))));
			_mm512_storeu_si512(slots + m, _mm512_maskz_compress_epi32(hit, value));
			m += static_cast<size_t>(PopCount64(hit));
		}
#endif
		for (; i < n; i++) {
			const auto key = static_cast<uint64_t>(keys[i]);
			const bool in_range = key < limit;
			const uint32_t word = in_range ? words_[key >> 4] : 0;
			const auto value = static_cast<int32_t>((word >> ((key & 15) << 1)) & 3);
			rows[m] = static_cast<uint32_t>(i);
			slots[m] = value;
			m += value != 0 ? 1 : 0;
		}
		return m;
	}

private:
	std::vector<uint32_t> words_;
	std::unordered_map<int64_t, uint8_t> overflow_;
};

class SupplierNations {
public:
	void Store(int64_t suppkey, int32_t nationkey) {
		if (suppkey >= 0 && suppkey < kDenseKeyLimit) {
			if (static_cast<size_t>(suppkey) >= dense_.size()) {
				dense_.resize(std::max(static_cast<size_t>(suppkey) + 1, dense_.size() * 2), kAbsentNation);
			}
			dense_[static_cast<size_t>(suppkey)] = nationkey;
			return;
		}
		overflow_[suppkey] = nationkey;
	}

	int32_t Get(int64_t suppkey) const {
		if (static_cast<uint64_t>(suppkey) < dense_.size()) {
			return dense_[static_cast<size_t>(suppkey)];
		}
		if (overflow_.empty()) {
			return kAbsentNation;
		}
		const auto it = overflow_.find(suppkey);
		return it == overflow_.end() ? kAbsentNation : it->second;
	}

private:
	std::vector<int32_t> dense_;
	std::unordered_map<int64_t, int32_t> overflow_;
};

struct Tables {
	KeyBitmap parts;
	KeyBitmap customers;
	OrderSlotMap orders;
	SupplierNations suppliers;
};

inline void AddVolume(YearVolumes &volumes, int slot, int32_t supp_nation, int32_t target_nation, int64_t extendedprice,
                      int64_t discount) {
	const acc_t value = static_cast<acc_t>(extendedprice) * static_cast<acc_t>(100 - discount);
	auto &state = volumes[static_cast<size_t>(slot)];
	state.total_volume += value;
	if (target_nation >= 0 && supp_nation == target_nation) {
		state.nation_volume += value;
	}
}

//! Scalar row step; also the generic (non-flat / NULL-aware) fallback.
inline void ProcessLineitemRow(const Tables &tables, int32_t target_nation, YearVolumes &volumes, int64_t partkey,
                               int64_t suppkey, int64_t orderkey, int64_t extendedprice, int64_t discount) {
	if (!tables.parts.Contains(partkey)) {
		return;
	}
	const auto slot_plus_one = tables.orders.Get(orderkey);
	if (slot_plus_one == 0) {
		return;
	}
	const auto supp_nation = tables.suppliers.Get(suppkey);
	if (supp_nation == kAbsentNation) {
		return;
	}
	AddVolume(volumes, static_cast<int>(slot_plus_one) - 1, supp_nation, target_nation, extendedprice, discount);
}

inline void ProcessLineitemChunk(const Tables &tables, int32_t target_nation, duckdb::DataChunk &chunk,
                                 YearVolumes &volumes) {
	const size_t n = chunk.size();
	auto &partkey_vector = chunk.data[Q8LineitemChunkLayout::PARTKEY_INDEX];
	auto &orderkey_vector = chunk.data[Q8LineitemChunkLayout::ORDERKEY_INDEX];
	auto &suppkey_vector = chunk.data[Q8LineitemChunkLayout::SUPPKEY_INDEX];
	auto &extendedprice_vector = chunk.data[Q8LineitemChunkLayout::EXTENDEDPRICE_INDEX];
	auto &discount_vector = chunk.data[Q8LineitemChunkLayout::DISCOUNT_INDEX];
	if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(orderkey_vector) && IsFlatAllValid(suppkey_vector) &&
	    IsFlatAllValid(extendedprice_vector) && IsFlatAllValid(discount_vector)) {
		const auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
		const auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		const auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
		const auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vector);
		const auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vector);

		uint32_t part_rows[STANDARD_VECTOR_SIZE + simd::kSelSlack];
		const size_t part_hits = tables.parts.Probe(partkeys, n, part_rows);
		if (part_hits == 0) {
			return;
		}
		int64_t keys[STANDARD_VECTOR_SIZE + simd::kSelSlack];
		for (size_t k = 0; k < part_hits; k++) {
			keys[k] = orderkeys[part_rows[k]];
		}
		uint32_t order_rows[STANDARD_VECTOR_SIZE + simd::kSelSlack];
		int32_t slots[STANDARD_VECTOR_SIZE + simd::kSelSlack];
		const size_t order_hits = tables.orders.Probe(keys, part_hits, order_rows, slots);
		for (size_t j = 0; j < order_hits; j++) {
			const auto row = part_rows[order_rows[j]];
			const auto supp_nation = tables.suppliers.Get(suppkeys[row]);
			if (supp_nation == kAbsentNation) {
				continue;
			}
			AddVolume(volumes, slots[j] - 1, supp_nation, target_nation, extendedprices[row], discounts[row]);
		}
		return;
	}
	duckdb::UnifiedVectorFormat partkey_format;
	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat suppkey_format;
	duckdb::UnifiedVectorFormat extendedprice_format;
	duckdb::UnifiedVectorFormat discount_format;
	partkey_vector.ToUnifiedFormat(n, partkey_format);
	orderkey_vector.ToUnifiedFormat(n, orderkey_format);
	suppkey_vector.ToUnifiedFormat(n, suppkey_format);
	extendedprice_vector.ToUnifiedFormat(n, extendedprice_format);
	discount_vector.ToUnifiedFormat(n, discount_format);
	const auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	const auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	const auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
	const auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
	const auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
	for (size_t row = 0; row < n; row++) {
		const auto partkey_index = partkey_format.sel->get_index(row);
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto suppkey_index = suppkey_format.sel->get_index(row);
		const auto extendedprice_index = extendedprice_format.sel->get_index(row);
		const auto discount_index = discount_format.sel->get_index(row);
		if (!partkey_format.validity.RowIsValid(partkey_index) || !orderkey_format.validity.RowIsValid(orderkey_index) ||
		    !suppkey_format.validity.RowIsValid(suppkey_index) ||
		    !extendedprice_format.validity.RowIsValid(extendedprice_index) ||
		    !discount_format.validity.RowIsValid(discount_index)) {
			continue;
		}
		ProcessLineitemRow(tables, target_nation, volumes, partkeys[partkey_index], suppkeys[suppkey_index],
		                   orderkeys[orderkey_index], extendedprices[extendedprice_index], discounts[discount_index]);
	}
}

inline bool TypeHasSuffix(const duckdb::string_t &value, const std::string &suffix) {
	const size_t size = value.GetSize();
	if (size < suffix.size()) {
		return false;
	}
	return suffix.empty() || std::memcmp(value.GetData() + size - suffix.size(), suffix.data(), suffix.size()) == 0;
}

//! Appends the keys of parts whose p_type ends with `suffix`.
inline void CollectPartKeys(duckdb::DataChunk &chunk, const std::string &suffix, std::vector<int64_t> &keys) {
	duckdb::UnifiedVectorFormat partkey_format;
	duckdb::UnifiedVectorFormat type_format;
	chunk.data[Q8PartChunkLayout::PARTKEY_INDEX].ToUnifiedFormat(chunk.size(), partkey_format);
	chunk.data[Q8PartChunkLayout::TYPE_INDEX].ToUnifiedFormat(chunk.size(), type_format);
	const auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	const auto types = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(type_format);
	const bool all_valid = partkey_format.validity.AllValid() && type_format.validity.AllValid();
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto partkey_index = partkey_format.sel->get_index(row);
		const auto type_index = type_format.sel->get_index(row);
		if (!all_valid && (!partkey_format.validity.RowIsValid(partkey_index) ||
		                   !type_format.validity.RowIsValid(type_index))) {
			continue;
		}
		if (TypeHasSuffix(types[type_index], suffix)) {
			keys.push_back(partkeys[partkey_index]);
		}
	}
}

template <class Fn>
inline void ForEachSupplier(duckdb::DataChunk &chunk, Fn &&fn) {
	duckdb::UnifiedVectorFormat suppkey_format;
	duckdb::UnifiedVectorFormat nationkey_format;
	chunk.data[Q8SupplierChunkLayout::SUPPKEY_INDEX].ToUnifiedFormat(chunk.size(), suppkey_format);
	chunk.data[Q8SupplierChunkLayout::NATIONKEY_INDEX].ToUnifiedFormat(chunk.size(), nationkey_format);
	const auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
	const auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto suppkey_index = suppkey_format.sel->get_index(row);
		const auto nationkey_index = nationkey_format.sel->get_index(row);
		if (!suppkey_format.validity.RowIsValid(suppkey_index) || !nationkey_format.validity.RowIsValid(nationkey_index)) {
			continue;
		}
		fn(suppkeys[suppkey_index], nationkeys[nationkey_index]);
	}
}

//! Appends the keys of customers whose nation is in the region.
inline void CollectCustomerKeys(duckdb::DataChunk &chunk, const std::array<bool, 25> &region_nations,
                                std::vector<int64_t> &keys) {
	duckdb::UnifiedVectorFormat custkey_format;
	duckdb::UnifiedVectorFormat nationkey_format;
	chunk.data[Q8CustomerChunkLayout::CUSTKEY_INDEX].ToUnifiedFormat(chunk.size(), custkey_format);
	chunk.data[Q8CustomerChunkLayout::NATIONKEY_INDEX].ToUnifiedFormat(chunk.size(), nationkey_format);
	const auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
	const auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto custkey_index = custkey_format.sel->get_index(row);
		const auto nationkey_index = nationkey_format.sel->get_index(row);
		if (!custkey_format.validity.RowIsValid(custkey_index) || !nationkey_format.validity.RowIsValid(nationkey_index)) {
			continue;
		}
		const auto nationkey = nationkeys[nationkey_index];
		if (nationkey < 0 || nationkey >= kMaxNationSlots || !region_nations[static_cast<size_t>(nationkey)]) {
			continue;
		}
		keys.push_back(custkeys[custkey_index]);
	}
}

struct OrderSlot {
	int64_t orderkey;
	int32_t slot;
};

//! Appends (orderkey, year slot) for orders of region customers placed in 1995 or 1996.
inline void CollectOrders(duckdb::DataChunk &chunk, const KeyBitmap &customers, std::vector<OrderSlot> &out) {
	const int32_t kYear1995 = duckdb::Date::FromDate(1995, 1, 1).days;
	const int32_t kYear1996 = duckdb::Date::FromDate(1996, 1, 1).days;
	const int32_t kYear1997 = duckdb::Date::FromDate(1997, 1, 1).days;
	auto &orderkey_vector = chunk.data[Q8OrdersChunkLayout::ORDERKEY_INDEX];
	auto &custkey_vector = chunk.data[Q8OrdersChunkLayout::CUSTKEY_INDEX];
	auto &orderdate_vector = chunk.data[Q8OrdersChunkLayout::ORDERDATE_INDEX];
	const size_t n = chunk.size();
	if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(custkey_vector) && IsFlatAllValid(orderdate_vector)) {
		const auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		const auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
		const auto orderdates = duckdb::FlatVector::GetData<int32_t>(orderdate_vector);
		uint32_t rows[STANDARD_VECTOR_SIZE + simd::kSelSlack];
		const size_t hits = customers.Probe(custkeys, n, rows);
		for (size_t j = 0; j < hits; j++) {
			const auto row = rows[j];
			const int32_t date = orderdates[row];
			if (date >= kYear1995 && date < kYear1997) {
				out.push_back(OrderSlot {orderkeys[row], date >= kYear1996 ? 1 : 0});
			}
		}
		return;
	}
	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat custkey_format;
	duckdb::UnifiedVectorFormat orderdate_format;
	orderkey_vector.ToUnifiedFormat(n, orderkey_format);
	custkey_vector.ToUnifiedFormat(n, custkey_format);
	orderdate_vector.ToUnifiedFormat(n, orderdate_format);
	const auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	const auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
	const auto orderdates = duckdb::UnifiedVectorFormat::GetData<int32_t>(orderdate_format);
	if (orderkey_format.validity.AllValid() && custkey_format.validity.AllValid() &&
	    orderdate_format.validity.AllValid()) {
		// Typical post-filter layout: sliced (dictionary) vectors without NULLs.  Probe the customer bitmap in
		// batches over the gathered customer keys.
		int64_t keys[STANDARD_VECTOR_SIZE + simd::kSelSlack];
		for (size_t row = 0; row < n; row++) {
			keys[row] = custkeys[custkey_format.sel->get_index(row)];
		}
		uint32_t rows[STANDARD_VECTOR_SIZE + simd::kSelSlack];
		const size_t hits = customers.Probe(keys, n, rows);
		for (size_t j = 0; j < hits; j++) {
			const auto row = rows[j];
			const int32_t date = orderdates[orderdate_format.sel->get_index(row)];
			if (date >= kYear1995 && date < kYear1997) {
				out.push_back(OrderSlot {orderkeys[orderkey_format.sel->get_index(row)], date >= kYear1996 ? 1 : 0});
			}
		}
		return;
	}
	for (size_t row = 0; row < n; row++) {
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto custkey_index = custkey_format.sel->get_index(row);
		const auto orderdate_index = orderdate_format.sel->get_index(row);
		if (!orderkey_format.validity.RowIsValid(orderkey_index) || !custkey_format.validity.RowIsValid(custkey_index) ||
		    !orderdate_format.validity.RowIsValid(orderdate_index)) {
			continue;
		}
		if (!customers.Contains(custkeys[custkey_index])) {
			continue;
		}
		const int32_t date = orderdates[orderdate_index];
		if (date >= kYear1995 && date < kYear1997) {
			out.push_back(OrderSlot {orderkeys[orderkey_index], date >= kYear1996 ? 1 : 0});
		}
	}
}

inline void ApplyOrderSlots(OrderSlotMap &map, const std::vector<std::vector<OrderSlot>> &parts) {
	int64_t max_key = -1;
	for (const auto &part : parts) {
		for (const auto &entry : part) {
			max_key = std::max(max_key, entry.orderkey);
		}
	}
	map.Reserve(max_key);
	for (const auto &part : parts) {
		for (const auto &entry : part) {
			map.Set(entry.orderkey, entry.slot);
		}
	}
}

inline void ApplyKeys(KeyBitmap &bitmap, const std::vector<std::vector<int64_t>> &parts) {
	int64_t max_key = -1;
	for (const auto &part : parts) {
		for (const auto key : part) {
			max_key = std::max(max_key, key);
		}
	}
	bitmap.Reserve(max_key);
	for (const auto &part : parts) {
		for (const auto key : part) {
			bitmap.Set(key);
		}
	}
}

inline bool HasRegionNations(const Q8Parameters &params) {
	for (const auto flag : params.region_nations) {
		if (flag) {
			return true;
		}
	}
	return false;
}

} // namespace q8
} // namespace accelerator_codegen
