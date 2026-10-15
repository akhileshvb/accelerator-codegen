// Workload-agnostic SIMD primitives for the stateless kernels.
//
// Every primitive has an AVX-512 path (__AVX512F__ + __AVX512VL__), an AVX2 path
// and a scalar branch-free fallback, chosen at compile time; the base kernel
// sources are built with ACCEL_BASE_COPTS (default -march=native).
// Selection vectors are uint32_t row indexes in ascending order; every
// function that writes one requires the output buffer to hold n entries plus
// kSelSlack slack (vector compress-stores may write past the logical end).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__AVX2__) || defined(__AVX512F__) || defined(__SSE4_2__)
#include <immintrin.h>
#endif

#define ACCEL_LIKELY(x) __builtin_expect(!!(x), 1)
#define ACCEL_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define ACCEL_RESTRICT __restrict__
#define ACCEL_PREFETCH(addr) __builtin_prefetch((addr), 0, 3)
#define ACCEL_PREFETCH_W(addr) __builtin_prefetch((addr), 1, 3)

namespace accelerator_codegen {
namespace simd {

#if defined(__AVX512F__) && defined(__AVX512VL__)
#define ACCEL_SIMD_AVX512 1
inline constexpr const char *kIsa = "avx512";
#elif defined(__AVX2__)
#define ACCEL_SIMD_AVX2 1
inline constexpr const char *kIsa = "avx2";
#else
inline constexpr const char *kIsa = "scalar";
#endif

//! Extra entries a selection buffer must have beyond its logical capacity.
inline constexpr std::size_t kSelSlack = 16;

namespace detail {

#if defined(ACCEL_SIMD_AVX2)
//! For an 8-bit lane mask, the packed lane indexes of set bits (for permutevar8x32).
struct CompressLut8 {
	alignas(32) uint32_t idx[256][8];
	uint8_t count[256];
	constexpr CompressLut8() : idx(), count() {
		for (int m = 0; m < 256; m++) {
			int k = 0;
			for (int b = 0; b < 8; b++) {
				if (m & (1 << b)) {
					idx[m][k++] = static_cast<uint32_t>(b);
				}
			}
			count[m] = static_cast<uint8_t>(k);
		}
	}
};
inline constexpr CompressLut8 kCompressLut8 {};

//! Append (base + lane) for each set bit of mask8 to out; returns new count.
inline std::size_t CompressAppend8(uint32_t base, unsigned mask8, uint32_t *out, std::size_t count) {
	const __m256i lanes = _mm256_load_si256(reinterpret_cast<const __m256i *>(kCompressLut8.idx[mask8]));
	const __m256i values = _mm256_add_epi32(lanes, _mm256_set1_epi32(static_cast<int>(base)));
	_mm256_storeu_si256(reinterpret_cast<__m256i *>(out + count), values);
	return count + kCompressLut8.count[mask8];
}

//! Same, but the 8 candidate values come from a vector (refine an existing selection).
inline std::size_t CompressAppendValues8(__m256i values, unsigned mask8, uint32_t *out, std::size_t count) {
	const __m256i lanes = _mm256_load_si256(reinterpret_cast<const __m256i *>(kCompressLut8.idx[mask8]));
	_mm256_storeu_si256(reinterpret_cast<__m256i *>(out + count), _mm256_permutevar8x32_epi32(values, lanes));
	return count + kCompressLut8.count[mask8];
}
#endif

} // namespace detail

//! out_sel <- { i in [0,n) : lo <= v[i] <= hi }.  Returns the selected count.
inline std::size_t SelectBetweenI32(const int32_t *ACCEL_RESTRICT v, std::size_t n, int32_t lo, int32_t hi,
                                    uint32_t *ACCEL_RESTRICT out_sel) {
	std::size_t count = 0;
	std::size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
	const __m512i vlo = _mm512_set1_epi32(lo), vhi = _mm512_set1_epi32(hi);
	const __m512i iota = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
	for (; i + 16 <= n; i += 16) {
		const __m512i x = _mm512_loadu_si512(v + i);
		const __mmask16 m = _mm512_cmpge_epi32_mask(x, vlo) & _mm512_cmple_epi32_mask(x, vhi);
		const __m512i idx = _mm512_add_epi32(iota, _mm512_set1_epi32(static_cast<int>(i)));
		_mm512_mask_compressstoreu_epi32(out_sel + count, m, idx);
		count += static_cast<std::size_t>(__builtin_popcount(m));
	}
#elif defined(ACCEL_SIMD_AVX2)
	const __m256i vlo = _mm256_set1_epi32(lo == INT32_MIN ? lo : lo - 1), vhi = _mm256_set1_epi32(hi);
	for (; i + 8 <= n; i += 8) {
		const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(v + i));
		// lo <= x  <=>  x > lo-1 (lo == INT32_MIN handled by the scalar tail semantics below)
		const __m256i ge = lo == INT32_MIN ? _mm256_set1_epi32(-1) : _mm256_cmpgt_epi32(x, vlo);
		const __m256i le = _mm256_andnot_si256(_mm256_cmpgt_epi32(x, vhi), _mm256_set1_epi32(-1));
		const unsigned m = static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_and_si256(ge, le))));
		count = detail::CompressAppend8(static_cast<uint32_t>(i), m, out_sel, count);
	}
#endif
	for (; i < n; i++) {
		out_sel[count] = static_cast<uint32_t>(i);
		count += (v[i] >= lo) & (v[i] <= hi);
	}
	return count;
}

//! out_sel <- { i in [0,n) : lo <= v[i] <= hi }.  Returns the selected count.
inline std::size_t SelectBetweenI64(const int64_t *ACCEL_RESTRICT v, std::size_t n, int64_t lo, int64_t hi,
                                    uint32_t *ACCEL_RESTRICT out_sel) {
	std::size_t count = 0;
	std::size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
	const __m512i vlo = _mm512_set1_epi64(lo), vhi = _mm512_set1_epi64(hi);
	const __m256i iota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
	for (; i + 8 <= n; i += 8) {
		const __m512i x = _mm512_loadu_si512(v + i);
		const __mmask8 m = _mm512_cmpge_epi64_mask(x, vlo) & _mm512_cmple_epi64_mask(x, vhi);
		const __m256i idx = _mm256_add_epi32(iota, _mm256_set1_epi32(static_cast<int>(i)));
		_mm256_mask_compressstoreu_epi32(out_sel + count, m, idx);
		count += static_cast<std::size_t>(__builtin_popcount(m));
	}
#endif
	for (; i < n; i++) {
		out_sel[count] = static_cast<uint32_t>(i);
		count += (v[i] >= lo) & (v[i] <= hi);
	}
	return count;
}

//! Keep the entries of sel (length n) whose v[sel[k]] lies in [lo, hi]; writes
//! them to out_sel (may alias sel).  Returns the kept count.
inline std::size_t RefineBetweenI32(const int32_t *ACCEL_RESTRICT v, const uint32_t *sel, std::size_t n, int32_t lo,
                                    int32_t hi, uint32_t *out_sel) {
	std::size_t count = 0;
	std::size_t k = 0;
#if defined(ACCEL_SIMD_AVX512)
	const __m512i vlo = _mm512_set1_epi32(lo), vhi = _mm512_set1_epi32(hi);
	for (; k + 16 <= n; k += 16) {
		const __m512i idx = _mm512_loadu_si512(sel + k);
		const __m512i x = _mm512_i32gather_epi32(idx, v, 4);
		const __mmask16 m = _mm512_cmpge_epi32_mask(x, vlo) & _mm512_cmple_epi32_mask(x, vhi);
		_mm512_mask_compressstoreu_epi32(out_sel + count, m, idx);
		count += static_cast<std::size_t>(__builtin_popcount(m));
	}
#endif
	for (; k < n; k++) {
		const uint32_t r = sel[k];
		out_sel[count] = r;
		count += (v[r] >= lo) & (v[r] <= hi);
	}
	return count;
}

//! Keep the entries of sel (length n) whose v[sel[k]] lies in [lo, hi]; writes
//! them to out_sel (may alias sel).  Returns the kept count.
inline std::size_t RefineBetweenI64(const int64_t *ACCEL_RESTRICT v, const uint32_t *sel, std::size_t n, int64_t lo,
                                    int64_t hi, uint32_t *out_sel) {
	std::size_t count = 0;
	std::size_t k = 0;
#if defined(ACCEL_SIMD_AVX512)
	const __m512i vlo = _mm512_set1_epi64(lo), vhi = _mm512_set1_epi64(hi);
	for (; k + 8 <= n; k += 8) {
		const __m256i idx = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(sel + k));
		const __m512i x = _mm512_i32gather_epi64(idx, v, 8);
		const __mmask8 m = _mm512_cmpge_epi64_mask(x, vlo) & _mm512_cmple_epi64_mask(x, vhi);
		_mm256_mask_compressstoreu_epi32(out_sel + count, m, idx);
		count += static_cast<std::size_t>(__builtin_popcount(m));
	}
#endif
	for (; k < n; k++) {
		const uint32_t r = sel[k];
		out_sel[count] = r;
		count += (v[r] >= lo) & (v[r] <= hi);
	}
	return count;
}

//! out_sel <- { i in [0,n) : bytes[i] != 0 }  (e.g. a per-row predicate byte map).
inline std::size_t SelectNonZeroU8(const uint8_t *ACCEL_RESTRICT bytes, std::size_t n,
                                   uint32_t *ACCEL_RESTRICT out_sel) {
	std::size_t count = 0;
	std::size_t i = 0;
#if defined(ACCEL_SIMD_AVX512) && defined(__AVX512BW__)
	const __m512i iota = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
	for (; i + 16 <= n; i += 16) {
		const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(bytes + i));
		const __mmask16 m = _mm_test_epi8_mask(b, b);
		const __m512i idx = _mm512_add_epi32(iota, _mm512_set1_epi32(static_cast<int>(i)));
		_mm512_mask_compressstoreu_epi32(out_sel + count, m, idx);
		count += static_cast<std::size_t>(__builtin_popcount(m));
	}
#elif defined(ACCEL_SIMD_AVX2)
	for (; i + 8 <= n; i += 8) {
		uint64_t word;
		std::memcpy(&word, bytes + i, 8);
		const __m128i b = _mm_cvtsi64_si128(static_cast<long long>(word));
		const unsigned zero = static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(b, _mm_setzero_si128()))) & 0xFF;
		count = detail::CompressAppend8(static_cast<uint32_t>(i), (~zero) & 0xFF, out_sel, count);
	}
#endif
	for (; i < n; i++) {
		out_sel[count] = static_cast<uint32_t>(i);
		count += bytes[i] != 0;
	}
	return count;
}

//! Sum of v[0..n) as int64 (caller guarantees no overflow for the chunk).
inline int64_t SumI64(const int64_t *ACCEL_RESTRICT v, std::size_t n) {
	std::size_t i = 0;
	int64_t total = 0;
#if defined(ACCEL_SIMD_AVX512)
	__m512i acc = _mm512_setzero_si512();
	for (; i + 8 <= n; i += 8) {
		acc = _mm512_add_epi64(acc, _mm512_loadu_si512(v + i));
	}
	total = _mm512_reduce_add_epi64(acc);
#elif defined(ACCEL_SIMD_AVX2)
	__m256i acc = _mm256_setzero_si256();
	for (; i + 4 <= n; i += 4) {
		acc = _mm256_add_epi64(acc, _mm256_loadu_si256(reinterpret_cast<const __m256i *>(v + i)));
	}
	alignas(32) int64_t lanes[4];
	_mm256_store_si256(reinterpret_cast<__m256i *>(lanes), acc);
	total = lanes[0] + lanes[1] + lanes[2] + lanes[3];
#endif
	for (; i < n; i++) {
		total += v[i];
	}
	return total;
}

//! 64-bit hash for integer join/group keys (CRC32C when SSE4.2 is enabled,
//! otherwise a multiplicative mix).  Good low bits; use `& (pow2 - 1)`.
inline uint64_t HashU64(uint64_t key) {
#if defined(__SSE4_2__)
	const uint64_t lo = _mm_crc32_u64(0x9E3779B9u, key);
	const uint64_t hi = _mm_crc32_u64(0x85EBCA6Bu, key);
	return (hi << 32) | lo;
#else
	key ^= key >> 33;
	key *= 0xff51afd7ed558ccdULL;
	key ^= key >> 33;
	key *= 0xc4ceb9fe1a85ec53ULL;
	key ^= key >> 33;
	return key;
#endif
}

} // namespace simd
} // namespace accelerator_codegen
