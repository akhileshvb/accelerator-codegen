#pragma once

// Query-local SIMD helper for TPC-H Q4: for every row with lhs[i] < rhs[i] set bit keys[i] in a
// bitmap.  Branch-free (the predicate is data-random, ~60% selective, so branches mispredict).
#include <cstddef>
#include <cstdint>

#include "prims/simd.hpp"

namespace accelerator_codegen {
namespace q4simd {

//! Largest key in [0, max_dense_key] among keys[0..n) (0 if none): used to grow dense structures
//! to the real key range before marking.  Out-of-range/negative keys are ignored (overflow path).
inline uint64_t MaxDenseKey(const int64_t *ACCEL_RESTRICT keys, std::size_t n, uint64_t max_dense_key) {
	uint64_t best = 0;
	for (std::size_t i = 0; i < n; i++) {
		const uint64_t k = static_cast<uint64_t>(keys[i]);
		const uint64_t v = k <= max_dense_key ? k : 0;
		best = v > best ? v : best;
	}
	return best;
}

//! words must hold at least limit_keys bits.  Keys outside [0, limit_keys) of qualifying rows are
//! handed to slow(key) (overflow path); non-qualifying rows are ignored whatever their key.
template <class SlowFn>
inline void MarkLess(const int64_t *ACCEL_RESTRICT keys, const int32_t *ACCEL_RESTRICT lhs,
                     const int32_t *ACCEL_RESTRICT rhs, std::size_t n, uint64_t *ACCEL_RESTRICT words,
                     uint64_t limit_keys, SlowFn &&slow) {
	std::size_t i = 0;
	auto scalar_row = [&](std::size_t r) {
		const uint64_t q = lhs[r] < rhs[r] ? 1 : 0;
		const uint64_t key = static_cast<uint64_t>(keys[r]);
		if (ACCEL_UNLIKELY(key >= limit_keys)) {
			if (q) {
				slow(keys[r]);
			}
			return;
		}
		words[key >> 6] |= q << (key & 63);
	};
#if defined(ACCEL_SIMD_AVX512)
	const __m512i vlimit = _mm512_set1_epi64(static_cast<long long>(limit_keys));
	// One 16-row block: SIMD compare + range check, then branch-free bit sets.
	auto block = [&](std::size_t base) {
		const __m512i c = _mm512_loadu_si512(lhs + base);
		const __m512i r = _mm512_loadu_si512(rhs + base);
		const unsigned m = _mm512_cmplt_epi32_mask(c, r);
		const __m512i k0 = _mm512_loadu_si512(keys + base);
		const __m512i k1 = _mm512_loadu_si512(keys + base + 8);
		const unsigned bad = _mm512_cmpge_epu64_mask(k0, vlimit) | _mm512_cmpge_epu64_mask(k1, vlimit);
		if (ACCEL_UNLIKELY(bad != 0)) {
			for (std::size_t j = 0; j < 16; j++) {
				scalar_row(base + j);
			}
			return;
		}
#pragma GCC unroll 16
		for (unsigned j = 0; j < 16; j++) {
			const uint64_t key = static_cast<uint64_t>(keys[base + j]);
			words[key >> 6] |= static_cast<uint64_t>((m >> j) & 1u) << (key & 63);
		}
	};
	for (; i + 16 <= n; i += 16) {
		block(i);
	}
#endif
	for (; i < n; i++) {
		scalar_row(i);
	}
}

} // namespace q4simd
} // namespace accelerator_codegen
