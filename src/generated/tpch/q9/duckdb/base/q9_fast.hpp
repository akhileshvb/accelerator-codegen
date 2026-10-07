// Query-local low-level helpers shared by the single-threaded and parallel Q9 kernels.
// Everything here is per-call state (owned by the kernel instance / worker); nothing persists.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/runtime/simd.hpp"
#include "duckdb/common/types/date.hpp"

namespace accelerator_codegen {
namespace q9fast {

//! Hardware sub-block size used for stack scratch buffers (DuckDB vectors are <= 2048 rows; larger inputs
//! are processed in several blocks).
inline constexpr std::size_t kBlock = 2048;

//! sel <- { i in [0,n) : keys[i] is a member of the bitmap }.  `words` is the bitmap as little-endian
//! 32-bit words covering `nbits` bits; keys outside [0,nbits) are decided by `outside(key)`.
//! sel needs n + simd::kSelSlack entries.  Returns the number of selected rows (ascending order).
template <class OutsideFn>
inline std::size_t SelectInBitmap(const int64_t *ACCEL_RESTRICT keys, std::size_t n, const uint32_t *words,
                                  uint64_t nbits, uint32_t *ACCEL_RESTRICT sel, OutsideFn &&outside) {
	std::size_t count = 0;
	std::size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
	const __m512i vnbits = _mm512_set1_epi64(static_cast<long long>(nbits));
	const __m512i v31 = _mm512_set1_epi64(31);
	const __m256i one = _mm256_set1_epi32(1);
	const __m256i iota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
	for (; i + 8 <= n; i += 8) {
		const __m512i k = _mm512_loadu_si512(keys + i);
		const __mmask8 inr = _mm512_cmplt_epu64_mask(k, vnbits);
		const __m512i widx = _mm512_srli_epi64(k, 5);
		const __m256i w = _mm512_mask_i64gather_epi32(_mm256_setzero_si256(), inr, widx, words, 4);
		const __m256i sh = _mm512_cvtepi64_epi32(_mm512_and_si512(k, v31));
		const __m256i bit = _mm256_and_si256(_mm256_srlv_epi32(w, sh), one);
		__mmask8 hit = _mm256_test_epi32_mask(bit, bit);
		unsigned oor = static_cast<unsigned>(static_cast<uint8_t>(~inr));
		while (ACCEL_UNLIKELY(oor != 0)) {
			const unsigned b = static_cast<unsigned>(__builtin_ctz(oor));
			oor &= oor - 1;
			if (outside(keys[i + b])) {
				hit = static_cast<__mmask8>(hit | (1u << b));
			}
		}
		const __m256i idx = _mm256_add_epi32(iota, _mm256_set1_epi32(static_cast<int>(i)));
		_mm256_mask_compressstoreu_epi32(sel + count, hit, idx);
		count += static_cast<std::size_t>(__builtin_popcount(hit));
	}
#endif
	for (; i < n; i++) {
		const int64_t key = keys[i];
		bool member;
		if (ACCEL_LIKELY(static_cast<uint64_t>(key) < nbits)) {
			member = (words[static_cast<std::size_t>(key) >> 5] >> (key & 31)) & 1u;
		} else {
			member = outside(key);
		}
		sel[count] = static_cast<uint32_t>(i);
		count += member ? 1 : 0;
	}
	return count;
}

//! Open-addressing (linear probing) uint64 -> int64 map.  Last Insert for a key wins.
class FlatU64Map {
public:
	FlatU64Map() {
		Rehash(1u << 10);
	}

	void Insert(uint64_t key, int64_t value) {
		if (ACCEL_UNLIKELY(key == kEmpty)) {
			has_max = true;
			max_value = value;
			return;
		}
		if (ACCEL_UNLIKELY((size + 1) * 2 > slots.size())) {
			Rehash(slots.size() * 2);
		}
		std::size_t pos = Slot(key);
		while (true) {
			Entry &e = slots[pos];
			if (e.key == key) {
				e.value = value;
				return;
			}
			if (e.key == kEmpty) {
				e.key = key;
				e.value = value;
				size++;
				return;
			}
			pos = (pos + 1) & mask;
		}
	}

	std::size_t HomeSlot(uint64_t key) const {
		return Slot(key);
	}

	void Prefetch(uint64_t key) const {
		ACCEL_PREFETCH(&slots[Slot(key)]);
	}

	const int64_t *Find(uint64_t key) const {
		if (ACCEL_UNLIKELY(key == kEmpty)) {
			return has_max ? &max_value : nullptr;
		}
		std::size_t pos = Slot(key);
		while (true) {
			const Entry &e = slots[pos];
			if (e.key == key) {
				return &e.value;
			}
			if (e.key == kEmpty) {
				return nullptr;
			}
			pos = (pos + 1) & mask;
		}
	}

	void MergeFrom(const FlatU64Map &other) {
		for (const auto &e : other.slots) {
			if (e.key != kEmpty) {
				Insert(e.key, e.value);
			}
		}
		if (other.has_max) {
			Insert(kEmpty, other.max_value);
		}
	}

	bool Empty() const {
		return size == 0 && !has_max;
	}

	std::size_t Size() const {
		return size + (has_max ? 1 : 0);
	}

private:
	static constexpr uint64_t kEmpty = std::numeric_limits<uint64_t>::max();
	struct Entry {
		uint64_t key;
		int64_t value;
	};

	std::size_t Slot(uint64_t key) const {
		return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> shift);
	}

	void Rehash(std::size_t new_slots) {
		std::vector<Entry> old;
		old.swap(slots);
		slots.assign(new_slots, Entry {kEmpty, 0});
		mask = new_slots - 1;
		shift = 64 - static_cast<unsigned>(__builtin_ctzll(new_slots));
		size = 0;
		for (const auto &e : old) {
			if (e.key != kEmpty) {
				Insert(e.key, e.value);
			}
		}
	}

	std::vector<Entry> slots;
	std::size_t mask = 0;
	unsigned shift = 0;
	std::size_t size = 0;
	bool has_max = false;
	int64_t max_value = 0;
};

//! Year classification of order dates in 1992..1998 via 8 calendar boundaries (Jan 1 of 1992..1999).
//! code 0 = absent, 1..7 = year 1991+code, 255 = some other year (kept exactly in a side map).
struct YearCoder {
	static constexpr int kBase = 1992;
	static constexpr int kYears = 7;
	int32_t bound[kYears + 1];
	YearCoder() {
		for (int i = 0; i <= kYears; i++) {
			bound[i] = duckdb::Date::FromDate(kBase + i, 1, 1).days;
		}
	}
	uint8_t Code(int32_t days) const {
		if (days < bound[0] || days >= bound[kYears]) {
			return 255;
		}
		unsigned c = 1;
		for (int i = 1; i < kYears; i++) {
			c += days >= bound[i] ? 1u : 0u;
		}
		return static_cast<uint8_t>(c);
	}
	//! out[i] = Code(days[i]) for i in [0,n)
	void FillCodes(const int32_t *ACCEL_RESTRICT days, std::size_t n, uint8_t *ACCEL_RESTRICT out) const {
		std::size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
		const __m512i b0 = _mm512_set1_epi32(bound[0]);
		const __m512i b7 = _mm512_set1_epi32(bound[kYears]);
		const __m512i one = _mm512_set1_epi32(1);
		const __m512i other = _mm512_set1_epi32(255);
		for (; i + 16 <= n; i += 16) {
			const __m512i d = _mm512_loadu_si512(days + i);
			__m512i c = one;
			for (int b = 1; b < kYears; b++) {
				c = _mm512_mask_add_epi32(c, _mm512_cmpge_epi32_mask(d, _mm512_set1_epi32(bound[b])), c, one);
			}
			const __mmask16 inr = _mm512_cmpge_epi32_mask(d, b0) & _mm512_cmplt_epi32_mask(d, b7);
			c = _mm512_mask_blend_epi32(inr, other, c);
			_mm_storeu_si128(reinterpret_cast<__m128i *>(out + i), _mm512_cvtepi32_epi8(c));
		}
#endif
		for (; i < n; i++) {
			out[i] = Code(days[i]);
		}
	}
	static int64_t YearOfCode(uint8_t code) {
		return static_cast<int64_t>(kBase - 1) + code;
	}
};

//! orderkey -> year.  Dense byte codes plus exact side maps for rare cases.
class OrderYears {
public:
	static constexpr uint8_t kAbsent = 0;
	static constexpr uint8_t kOther = 255;

	void Reserve(int64_t min_capacity) {
		if (min_capacity > capacity) {
			Grow(min_capacity);
		}
	}

	int64_t Capacity() const {
		return capacity;
	}

	uint8_t *Codes() {
		return codes.data();
	}

	//! Exact insert (grows the dense array for keys <= 1e9).
	void Insert(int64_t orderkey, int64_t year) {
		if (orderkey >= capacity && orderkey <= 1'000'000'000) {
			Grow(orderkey + 1);
		}
		if (orderkey >= 0 && orderkey < capacity) {
			const int64_t rel = year - (YearCoder::kBase - 1);
			if (rel >= 1 && rel <= YearCoder::kYears) {
				codes[static_cast<std::size_t>(orderkey)] = static_cast<uint8_t>(rel);
				big_years.erase(orderkey);
			} else {
				codes[static_cast<std::size_t>(orderkey)] = kOther;
				big_years[orderkey] = year;
			}
			return;
		}
		overflow[orderkey] = year;
	}

	//! Dense-only insert of a precomputed code (no growth, no side maps); caller guarantees 0 <= key < capacity.
	void SetCode(int64_t orderkey, uint8_t code) {
		codes[static_cast<std::size_t>(orderkey)] = code;
	}

	bool Lookup(int64_t orderkey, int64_t &year_out) const {
		if (ACCEL_LIKELY(static_cast<uint64_t>(orderkey) < static_cast<uint64_t>(capacity))) {
			const uint8_t c = codes[static_cast<std::size_t>(orderkey)];
			if (ACCEL_LIKELY(c != kAbsent && c != kOther)) {
				year_out = YearCoder::YearOfCode(c);
				return true;
			}
			if (c == kAbsent) {
				return false;
			}
			auto it = big_years.find(orderkey);
			if (it == big_years.end()) {
				return false;
			}
			year_out = it->second;
			return true;
		}
		auto it = overflow.find(orderkey);
		if (it == overflow.end()) {
			return false;
		}
		year_out = it->second;
		return true;
	}

	void Prefetch(int64_t orderkey) const {
		if (static_cast<uint64_t>(orderkey) < static_cast<uint64_t>(capacity)) {
			ACCEL_PREFETCH(&codes[static_cast<std::size_t>(orderkey)]);
		}
	}

private:
	void Grow(int64_t min_capacity) {
		int64_t target = min_capacity;
		if (capacity > 0) {
			const int64_t doubled = capacity > (std::numeric_limits<int64_t>::max() / 2)
			                            ? std::numeric_limits<int64_t>::max()
			                            : capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		}
		codes.resize(static_cast<std::size_t>(target), kAbsent);
		capacity = target;
	}

	int64_t capacity = 0;
	std::vector<uint8_t> codes;
	std::unordered_map<int64_t, int64_t> big_years;
	std::unordered_map<int64_t, int64_t> overflow;
};

} // namespace q9fast
} // namespace accelerator_codegen
