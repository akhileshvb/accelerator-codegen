// Query-local low-level helpers shared by the single-threaded and parallel Q19 kernels.
// All state is per call (owned by the kernel instance); nothing persists across calls.
#pragma once

#include <cstddef>
#include <cstdint>

#include "accelerator_codegen/runtime/simd.hpp"

namespace accelerator_codegen {
namespace q19fast {

inline constexpr std::size_t kBlock = 2048;

//! sel <- { i in [0,n) : table[keys[i]] != 0 } where table has `table_size` bytes.  Keys outside the table are
//! decided by `outside(key)`.  The AVX-512 path gathers a dword per key, so it only trusts keys below
//! table_size - 3 (the last 3 bytes of the table are handled by the scalar/outside path).
//! sel needs n + simd::kSelSlack entries.  Returns the number of selected rows (ascending order).
template <class OutsideFn>
inline std::size_t SelectNonZeroBytes(const int64_t *ACCEL_RESTRICT keys, std::size_t n,
                                      const uint8_t *ACCEL_RESTRICT table, uint64_t table_size,
                                      uint32_t *ACCEL_RESTRICT sel, OutsideFn &&outside) {
	std::size_t count = 0;
	std::size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
	if (table_size >= 4) {
		const __m512i vlimit = _mm512_set1_epi64(static_cast<long long>(table_size - 3));
		const __m256i low_byte = _mm256_set1_epi32(0xFF);
		const __m256i iota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
		for (; i + 8 <= n; i += 8) {
			const __m512i k = _mm512_loadu_si512(keys + i);
			const __mmask8 inr = _mm512_cmplt_epu64_mask(k, vlimit);
			const __m256i w = _mm512_mask_i64gather_epi32(_mm256_setzero_si256(), inr, k, table, 1);
			const __m256i b = _mm256_and_si256(w, low_byte);
			__mmask8 hit = _mm256_test_epi32_mask(b, b);
			unsigned oor = static_cast<unsigned>(static_cast<uint8_t>(~inr));
			while (ACCEL_UNLIKELY(oor != 0)) {
				const unsigned bit = static_cast<unsigned>(__builtin_ctz(oor));
				oor &= oor - 1;
				if (outside(keys[i + bit])) {
					hit = static_cast<__mmask8>(hit | (1u << bit));
				}
			}
			const __m256i idx = _mm256_add_epi32(iota, _mm256_set1_epi32(static_cast<int>(i)));
			_mm256_mask_compressstoreu_epi32(sel + count, hit, idx);
			count += static_cast<std::size_t>(__builtin_popcount(hit));
		}
	}
#endif
	for (; i < n; i++) {
		const int64_t key = keys[i];
		bool member;
		if (ACCEL_LIKELY(static_cast<uint64_t>(key) < table_size)) {
			member = table[static_cast<std::size_t>(key)] != 0;
		} else {
			member = outside(key);
		}
		sel[count] = static_cast<uint32_t>(i);
		count += member ? 1 : 0;
	}
	return count;
}

} // namespace q19fast
} // namespace accelerator_codegen
