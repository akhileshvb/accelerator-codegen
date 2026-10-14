#pragma once

// Query-local SIMD helpers for TPC-H Q12 (shared by the single-threaded and parallel kernels).
//
// Lineitem chunks arrive as DICTIONARY vectors (a selection vector over a decoded child) on all five
// columns.  FilterDict evaluates, 16 rows at a time with AVX-512 gathers, the predicates
//   shipmode in (m1, m2) AND commitdate < receiptdate AND shipdate < commitdate [AND receipt in window]
// and emits only the (rare) surviving (orderkey, slot) pairs for the scalar dense-array probe.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "prims/simd.hpp"

namespace accelerator_codegen {
namespace q12simd {

//! Shipmode dictionary indexes must be below this to use the per-chunk LUT.
constexpr std::size_t kLutSize = 2048;
//! Buffer size for the LUT (gathers read 4 bytes at any valid index).
constexpr std::size_t kLutBytes = kLutSize + 4;
constexpr uint8_t kLutUnset = 0xFF;

//! Fill lut[idx] = classify(idx) for every shipmode dictionary index referenced by idx[0..n).
//! Returns false (caller falls back to the generic path) if an index is >= kLutSize.
template <class ClassifyFn>
inline bool BuildSlotLut(const uint32_t *idx, std::size_t n, uint8_t *lut, ClassifyFn &&classify) {
	std::memset(lut, kLutUnset, kLutBytes);
	for (std::size_t i = 0; i < n; i++) {
		const uint32_t s = idx[i];
		if (ACCEL_UNLIKELY(s >= kLutSize)) {
			return false;
		}
		if (lut[s] == kLutUnset) {
			lut[s] = classify(s);
		}
	}
	return true;
}

//! Writes the surviving rows' (orderkey, slot) to out_key/out_slot (capacity n) and returns the count.
//! sh_idx / c_idx / r_idx / s_idx / o_idx are the per-column selection indexes into the decoded
//! children (all non-null, length n).  slot values are lut entries (0 = no shipmode match).
inline std::size_t FilterDict(const uint32_t *ACCEL_RESTRICT o_idx, const uint32_t *ACCEL_RESTRICT sh_idx,
                              const uint32_t *ACCEL_RESTRICT c_idx, const uint32_t *ACCEL_RESTRICT r_idx,
                              const uint32_t *ACCEL_RESTRICT s_idx, std::size_t n, const int64_t *ACCEL_RESTRICT keys,
                              const uint8_t *ACCEL_RESTRICT lut, const int32_t *ACCEL_RESTRICT commit,
                              const int32_t *ACCEL_RESTRICT receipt, const int32_t *ACCEL_RESTRICT ship,
                              int32_t start, int32_t end, bool residual, int64_t *ACCEL_RESTRICT out_key,
                              uint8_t *ACCEL_RESTRICT out_slot) {
	std::size_t out = 0;
	std::size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
	const __m512i vstart = _mm512_set1_epi32(start), vend = _mm512_set1_epi32(end);
	const __m512i vff = _mm512_set1_epi32(0xFF);
	for (; i < n; i += 16) {
		const std::size_t rem = n - i;
		const __mmask16 lane = rem >= 16 ? static_cast<__mmask16>(0xFFFF) : static_cast<__mmask16>((1u << rem) - 1u);
		const __m512i ish = _mm512_maskz_loadu_epi32(lane, sh_idx + i);
		const __m512i slot = _mm512_and_si512(_mm512_mask_i32gather_epi32(_mm512_setzero_si512(), lane, ish, lut, 1), vff);
		__mmask16 m = _mm512_mask_cmpneq_epi32_mask(lane, slot, _mm512_setzero_si512());
		if (m == 0) {
			continue;
		}
		const __m512i ic = _mm512_maskz_loadu_epi32(m, c_idx + i);
		const __m512i ir = _mm512_maskz_loadu_epi32(m, r_idx + i);
		const __m512i is = _mm512_maskz_loadu_epi32(m, s_idx + i);
		const __m512i c = _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), m, ic, commit, 4);
		const __m512i r = _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), m, ir, receipt, 4);
		const __m512i s = _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), m, is, ship, 4);
		m = _mm512_mask_cmplt_epi32_mask(m, c, r);
		m = _mm512_mask_cmplt_epi32_mask(m, s, c);
		if (residual) {
			m = _mm512_mask_cmpge_epi32_mask(m, r, vstart);
			m = _mm512_mask_cmplt_epi32_mask(m, r, vend);
		}
		while (m) {
			const unsigned j = static_cast<unsigned>(__builtin_ctz(m));
			m &= m - 1;
			out_key[out] = keys[o_idx[i + j]];
			out_slot[out] = lut[sh_idx[i + j]];
			out++;
		}
	}
#endif
	for (; i < n; i++) {
		const uint8_t slot = lut[sh_idx[i]];
		const int32_t c = commit[c_idx[i]], r = receipt[r_idx[i]], s = ship[s_idx[i]];
		bool ok = (slot != 0) & (c < r) & (s < c);
		if (residual) {
			ok &= (r >= start) & (r < end);
		}
		out_key[out] = keys[o_idx[i]];
		out_slot[out] = slot;
		out += ok ? 1 : 0;
	}
	return out;
}

} // namespace q12simd
} // namespace accelerator_codegen
