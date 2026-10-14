// Query-local low-level helpers shared by the single-threaded and parallel Q5 kernels.
// All state is per call (owned by the kernel instance); nothing persists across calls.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "prims/simd.hpp"

namespace accelerator_codegen {
namespace q5fast {

inline constexpr std::size_t kBlock = 2048;

//! sel <- { i in [0,n) : keys[i] is a member of the bitmap }.  `words` is the bitmap as little-endian 32-bit
//! words covering `nbits` bits; keys outside [0,nbits) are decided by `outside(key)`.
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

//! Membership bitmap derived from a dense byte array: bit k is set iff bytes[k] != absent.
class ByteBitmap {
public:
	void Build(const uint8_t *bytes, std::size_t n, uint8_t absent) {
		nbits_ = n;
		words_.assign(((n + 63) / 64) * 2 + 2, 0);
		std::size_t i = 0;
#if defined(ACCEL_SIMD_AVX512) && defined(__AVX512BW__)
		const __m512i vabsent = _mm512_set1_epi8(static_cast<char>(absent));
		for (; i + 64 <= n; i += 64) {
			const uint64_t m = _mm512_cmpneq_epi8_mask(_mm512_loadu_si512(bytes + i), vabsent);
			words_[i / 32] = static_cast<uint32_t>(m);
			words_[i / 32 + 1] = static_cast<uint32_t>(m >> 32);
		}
#endif
		for (; i < n; i++) {
			words_[i >> 5] |= static_cast<uint32_t>(bytes[i] != absent) << (i & 31);
		}
	}

	const uint32_t *Words() const {
		return words_.data();
	}

	uint64_t Bits() const {
		return nbits_;
	}

private:
	uint64_t nbits_ = 0;
	std::vector<uint32_t> words_;
};

} // namespace q5fast
} // namespace accelerator_codegen
