// Query-local helpers shared by the Q16 single-threaded and parallel kernels.  All state is per
// kernel instance / per call; nothing here is static or cached.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include "prims/simd.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace q16 {

//! Hash of a byte string (8 bytes at a time, multiplicative mix).
inline uint64_t HashBytes(const char *data, size_t len) {
	uint64_t h = 0x9E3779B97F4A7C15ULL ^ (len * 0xff51afd7ed558ccdULL);
	size_t i = 0;
	for (; i + 8 <= len; i += 8) {
		uint64_t w;
		std::memcpy(&w, data + i, 8);
		h = (h ^ w) * 0xff51afd7ed558ccdULL;
		h ^= h >> 32;
	}
	if (i < len) {
		uint64_t w = 0;
		std::memcpy(&w, data + i, len - i);
		h = (h ^ w) * 0xc4ceb9fe1a85ec53ULL;
		h ^= h >> 32;
	}
	return h;
}

//! Dense-id interner for strings (open addressing, ids in insertion order).
class StringInterner {
public:
	StringInterner() : slots_(256, kEmpty) {
	}

	uint32_t Intern(const duckdb::string_t &value) {
		return Intern(value.GetData(), value.GetSize());
	}

	uint32_t Intern(const char *data, size_t len) {
		const uint64_t h = HashBytes(data, len);
		size_t mask = slots_.size() - 1;
		size_t pos = static_cast<size_t>(h) & mask;
		while (slots_[pos] != kEmpty) {
			const std::string &stored = values_[slots_[pos]];
			if (stored.size() == len && std::memcmp(stored.data(), data, len) == 0) {
				return slots_[pos];
			}
			pos = (pos + 1) & mask;
		}
		const uint32_t id = static_cast<uint32_t>(values_.size());
		values_.emplace_back(data, len);
		hashes_.push_back(h);
		slots_[pos] = id;
		if (values_.size() * 2 > slots_.size()) {
			Grow();
		}
		return id;
	}

	const std::string &Get(uint32_t id) const {
		return values_[id];
	}
	size_t Size() const {
		return values_.size();
	}

private:
	static constexpr uint32_t kEmpty = 0xFFFFFFFFu;
	void Grow() {
		std::vector<uint32_t> bigger(slots_.size() * 2, kEmpty);
		const size_t mask = bigger.size() - 1;
		for (uint32_t id = 0; id < values_.size(); id++) {
			size_t pos = static_cast<size_t>(hashes_[id]) & mask;
			while (bigger[pos] != kEmpty) {
				pos = (pos + 1) & mask;
			}
			bigger[pos] = id;
		}
		slots_.swap(bigger);
	}

	std::vector<uint32_t> slots_;
	std::vector<std::string> values_;
	std::vector<uint64_t> hashes_;
};

//! Map from a 64-bit key to a dense int32 id (open addressing, key ~0 reserved).
class KeyTable {
public:
	KeyTable() : keys_(1024, kEmpty), ids_(1024, 0) {
	}
	//! Returns the id for key, or -1 if absent.
	int32_t Find(uint64_t key) const {
		const size_t mask = keys_.size() - 1;
		size_t pos = Mix(key) & mask;
		while (keys_[pos] != kEmpty) {
			if (keys_[pos] == key) {
				return ids_[pos];
			}
			pos = (pos + 1) & mask;
		}
		return -1;
	}
	void Insert(uint64_t key, int32_t id) {
		if ((count_ + 1) * 2 > keys_.size()) {
			Grow();
		}
		Put(key, id);
		count_++;
	}

private:
	static constexpr uint64_t kEmpty = ~0ULL;
	static size_t Mix(uint64_t k) {
		k *= 0x9E3779B97F4A7C15ULL;
		return static_cast<size_t>(k >> 29);
	}
	void Put(uint64_t key, int32_t id) {
		const size_t mask = keys_.size() - 1;
		size_t pos = Mix(key) & mask;
		while (keys_[pos] != kEmpty) {
			pos = (pos + 1) & mask;
		}
		keys_[pos] = key;
		ids_[pos] = id;
	}
	void Grow() {
		std::vector<uint64_t> old_keys;
		std::vector<int32_t> old_ids;
		old_keys.swap(keys_);
		old_ids.swap(ids_);
		keys_.assign(old_keys.size() * 2, kEmpty);
		ids_.assign(old_keys.size() * 2, 0);
		for (size_t i = 0; i < old_keys.size(); i++) {
			if (old_keys[i] != kEmpty) {
				Put(old_keys[i], old_ids[i]);
			}
		}
	}

	std::vector<uint64_t> keys_;
	std::vector<int32_t> ids_;
	size_t count_ = 0;
};

//! Pack (brand_id, type_id, size) for KeyTable; size must be in [0, 255].
inline uint64_t GroupKey(uint32_t brand_id, uint32_t type_id, int32_t size) {
	return (static_cast<uint64_t>(brand_id) << 40) | (static_cast<uint64_t>(type_id) << 8) |
	       static_cast<uint64_t>(size);
}

//! sel <- rows i with sizes[i] in the allowed set.  table is 64 bytes (index = size, 0 outside
//! 1..50 in practice); out-of-range sizes are rejected.  sel needs n + kSelSlack entries.
inline size_t SelectAllowedSizes(const uint8_t *table64, const int32_t *ACCEL_RESTRICT sizes, size_t n,
                                 uint32_t *ACCEL_RESTRICT sel) {
	size_t count = 0;
	size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
	const __m512i iota = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
	const __m512i limit = _mm512_set1_epi32(51);
	const __m512i ff = _mm512_set1_epi32(0xFF);
	for (; i + 16 <= n; i += 16) {
		const __m512i s = _mm512_loadu_si512(sizes + i);
		const __mmask16 in = _mm512_cmplt_epu32_mask(s, limit);
		const __m512i g = _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), in, s, table64, 1);
		const __mmask16 hit = _mm512_mask_test_epi32_mask(in, g, ff);
		const __m512i idx = _mm512_add_epi32(iota, _mm512_set1_epi32(static_cast<int>(i)));
		_mm512_mask_compressstoreu_epi32(sel + count, hit, idx);
		count += static_cast<size_t>(__builtin_popcount(hit));
	}
#endif
	for (; i < n; i++) {
		const uint32_t s = static_cast<uint32_t>(sizes[i]);
		sel[count] = static_cast<uint32_t>(i);
		count += (s < 51) & (table64[s < 51 ? s : 0] != 0);
	}
	return count;
}

//! sel <- rows i with slot[partkeys[i]] != 0 (partkey in [0, slot_cap)) and the supplier not
//! excluded.  excluded needs >= 3 readable bytes past excluded_cap.
inline size_t SelectPartsuppHits(const int32_t *slot, size_t slot_cap, const int64_t *ACCEL_RESTRICT partkeys,
                                 const uint8_t *excluded, size_t excluded_cap,
                                 const int64_t *ACCEL_RESTRICT suppkeys, size_t n, uint32_t *ACCEL_RESTRICT sel) {
	size_t count = 0;
	size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
	const __m512i vslot_cap = _mm512_set1_epi64(static_cast<long long>(slot_cap));
	const __m512i vexc_cap = _mm512_set1_epi64(static_cast<long long>(excluded_cap));
	const __m256i iota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
	const __m256i ff = _mm256_set1_epi32(0xFF);
	for (; i + 8 <= n; i += 8) {
		const __m512i pk = _mm512_loadu_si512(partkeys + i);
		const __mmask8 in_p = _mm512_cmplt_epu64_mask(pk, vslot_cap);
		const __m256i gs = _mm512_mask_i64gather_epi32(_mm256_setzero_si256(), in_p, pk, slot, 4);
		const __mmask8 hit_p = _mm256_mask_test_epi32_mask(in_p, gs, gs);
		if (hit_p == 0) {
			continue;
		}
		const __m512i sk = _mm512_loadu_si512(suppkeys + i);
		const __mmask8 in_s = _mm512_mask_cmplt_epu64_mask(hit_p, sk, vexc_cap);
		const __m256i ge = _mm512_mask_i64gather_epi32(_mm256_setzero_si256(), in_s, sk, excluded, 1);
		const __mmask8 excl = _mm256_mask_test_epi32_mask(in_s, ge, ff);
		const __mmask8 hit = hit_p & static_cast<__mmask8>(~excl);
		const __m256i idx = _mm256_add_epi32(iota, _mm256_set1_epi32(static_cast<int>(i)));
		_mm256_mask_compressstoreu_epi32(sel + count, hit, idx);
		count += static_cast<size_t>(__builtin_popcount(hit));
	}
#endif
	for (; i < n; i++) {
		const uint64_t pk = static_cast<uint64_t>(partkeys[i]);
		const uint64_t sk = static_cast<uint64_t>(suppkeys[i]);
		const bool part_ok = pk < slot_cap && slot[pk] != 0;
		const bool excl = sk < excluded_cap && excluded[sk] != 0;
		sel[count] = static_cast<uint32_t>(i);
		count += part_ok & !excl;
	}
	return count;
}

//! LSD radix sort of 64-bit keys; only the low `bits` bits are significant.  tmp is scratch.
inline void RadixSortU64(std::vector<uint64_t> &keys, std::vector<uint64_t> &tmp, unsigned bits) {
	const size_t n = keys.size();
	if (n < 2) {
		return;
	}
	tmp.resize(n);
	constexpr unsigned kDigit = 11;
	uint64_t *src = keys.data();
	uint64_t *dst = tmp.data();
	for (unsigned shift = 0; shift < bits; shift += kDigit) {
		size_t counts[1u << kDigit] = {0};
		for (size_t i = 0; i < n; i++) {
			counts[(src[i] >> shift) & ((1u << kDigit) - 1)]++;
		}
		size_t sum = 0;
		for (size_t d = 0; d < (1u << kDigit); d++) {
			const size_t c = counts[d];
			counts[d] = sum;
			sum += c;
		}
		for (size_t i = 0; i < n; i++) {
			dst[counts[(src[i] >> shift) & ((1u << kDigit) - 1)]++] = src[i];
		}
		std::swap(src, dst);
	}
	if (src != keys.data()) {
		std::memcpy(keys.data(), src, n * sizeof(uint64_t));
	}
}

inline unsigned BitsFor(uint64_t max_value) {
	unsigned bits = 1;
	while (bits < 64 && (max_value >> bits) != 0) {
		bits++;
	}
	return bits;
}

//! keys = (group_id << 32) | suppkey.  Returns, per group, the number of distinct suppkeys.
inline void CountDistinctPerGroup(std::vector<uint64_t> &keys, std::vector<int64_t> &group_counts) {
	if (keys.empty()) {
		return;
	}
	uint64_t max_key = 0;
	for (const auto k : keys) {
		max_key = std::max(max_key, k);
	}
	std::vector<uint64_t> tmp;
	RadixSortU64(keys, tmp, BitsFor(max_key));
	uint64_t prev = ~0ULL;
	for (const auto k : keys) {
		if (k == prev) {
			continue;
		}
		prev = k;
		const size_t group = static_cast<size_t>(k >> 32);
		if (group < group_counts.size()) {
			group_counts[group] += 1;
		}
	}
}

struct OutRow {
	uint32_t brand_rank;
	uint32_t type_rank;
	int32_t size;
	int64_t supplier_cnt;
	uint32_t group_id;
};

inline bool BetterOutRow(const OutRow &a, const OutRow &b) {
	if (a.supplier_cnt != b.supplier_cnt) {
		return a.supplier_cnt > b.supplier_cnt;
	}
	if (a.brand_rank != b.brand_rank) {
		return a.brand_rank < b.brand_rank;
	}
	if (a.type_rank != b.type_rank) {
		return a.type_rank < b.type_rank;
	}
	return a.size < b.size;
}

//! rank[id] = position of the id's string in lexicographic (std::string) order.
inline std::vector<uint32_t> LexRanks(const StringInterner &interner) {
	std::vector<uint32_t> order(interner.Size());
	std::iota(order.begin(), order.end(), 0u);
	std::sort(order.begin(), order.end(),
	          [&](uint32_t a, uint32_t b) { return interner.Get(a) < interner.Get(b); });
	std::vector<uint32_t> rank(order.size());
	for (uint32_t pos = 0; pos < order.size(); pos++) {
		rank[order[pos]] = pos;
	}
	return rank;
}

inline void WriteString(duckdb::Vector &vec, duckdb::string_t *data, duckdb::idx_t row, const std::string &s) {
	data[row] = duckdb::StringVector::AddString(vec, s.data(), s.size());
}

} // namespace q16
} // namespace accelerator_codegen
