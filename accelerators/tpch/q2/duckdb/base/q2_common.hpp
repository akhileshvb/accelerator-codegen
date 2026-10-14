// Query-local SIMD helpers shared by the Q2 single-threaded and parallel kernels (no state).
#pragma once

#include <cstddef>
#include <cstdint>

#include "prims/simd.hpp"

namespace accelerator_codegen {
namespace q2 {

//! sel <- rows i in [0,n) with flagsA[keysA[i]] != 0 && flagsB[keysB[i]] != 0 (keys outside their
//! capacity count as 0).  Each flags buffer must have at least 3 readable bytes past its capacity
//! (the vector gathers read 4 bytes).  sel needs n + simd::kSelSlack entries.
inline size_t SelectBothFlagged(const uint8_t *flags_a, size_t cap_a, const int64_t *ACCEL_RESTRICT keys_a,
                                const uint8_t *flags_b, size_t cap_b, const int64_t *ACCEL_RESTRICT keys_b, size_t n,
                                uint32_t *ACCEL_RESTRICT sel) {
	size_t count = 0;
	size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
	const __m512i vcap_a = _mm512_set1_epi64(static_cast<long long>(cap_a));
	const __m512i vcap_b = _mm512_set1_epi64(static_cast<long long>(cap_b));
	const __m256i iota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
	const __m256i ff = _mm256_set1_epi32(0xFF);
	for (; i + 8 <= n; i += 8) {
		const __m512i ka = _mm512_loadu_si512(keys_a + i);
		const __mmask8 in_a = _mm512_cmplt_epu64_mask(ka, vcap_a);
		const __m256i ga = _mm512_mask_i64gather_epi32(_mm256_setzero_si256(), in_a, ka, flags_a, 1);
		const __mmask8 hit_a = _mm256_mask_test_epi32_mask(in_a, ga, ff);
		if (hit_a == 0) {
			continue;
		}
		const __m512i kb = _mm512_loadu_si512(keys_b + i);
		const __mmask8 in_b = _mm512_mask_cmplt_epu64_mask(hit_a, kb, vcap_b);
		const __m256i gb = _mm512_mask_i64gather_epi32(_mm256_setzero_si256(), in_b, kb, flags_b, 1);
		const __mmask8 hit = _mm256_mask_test_epi32_mask(in_b, gb, ff);
		const __m256i idx = _mm256_add_epi32(iota, _mm256_set1_epi32(static_cast<int>(i)));
		_mm256_mask_compressstoreu_epi32(sel + count, hit, idx);
		count += static_cast<size_t>(__builtin_popcount(hit));
	}
#endif
	for (; i < n; i++) {
		const uint64_t ka = static_cast<uint64_t>(keys_a[i]);
		const uint64_t kb = static_cast<uint64_t>(keys_b[i]);
		const bool ok = ka < cap_a && kb < cap_b && flags_a[ka] != 0 && flags_b[kb] != 0;
		sel[count] = static_cast<uint32_t>(i);
		count += ok;
	}
	return count;
}

} // namespace q2
} // namespace accelerator_codegen
