#pragma once

// Query-local "%part1%part2%" LIKE matcher with an AVX-512BW substring search
// ("SIMD-friendly strstr": first/last byte broadcast compare + memcmp of the middle).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "prims/simd.hpp"

namespace accelerator_codegen {
namespace q13 {

#if defined(__AVX512BW__) && defined(__AVX512F__) && defined(__AVX512VL__)
#define Q13_SIMD_STRSTR 1
#endif

//! First occurrence of needle[0..m) in hay[from..n), or n if none (m >= 1).
inline size_t FindFrom(const char *hay, size_t n, size_t from, const char *needle, size_t m) {
	if (n < m || from > n - m) {
		return n;
	}
	const size_t last_start = n - m; // inclusive
#if defined(Q13_SIMD_STRSTR)
	const __m512i first = _mm512_set1_epi8(needle[0]);
	const __m512i last = _mm512_set1_epi8(needle[m - 1]);
	for (size_t i = from; i <= last_start; i += 64) {
		const size_t positions = last_start - i + 1; // candidate starts remaining
		const size_t npos = positions < 64 ? positions : 64;
		const __mmask64 valid = npos == 64 ? ~0ULL : ((1ULL << npos) - 1);
		const size_t b_avail = n - (i + m - 1); // bytes available from i+m-1
		const __mmask64 bload = b_avail >= 64 ? ~0ULL : ((1ULL << b_avail) - 1);
		const __m512i a = _mm512_maskz_loadu_epi8(valid, hay + i);
		const __m512i b = _mm512_maskz_loadu_epi8(bload, hay + i + m - 1);
		uint64_t cand = _mm512_mask_cmpeq_epi8_mask(valid, a, first) & _mm512_cmpeq_epi8_mask(b, last);
		while (cand) {
			const unsigned bit = static_cast<unsigned>(__builtin_ctzll(cand));
			cand &= cand - 1;
			if (m <= 2 || std::memcmp(hay + i + bit + 1, needle + 1, m - 2) == 0) {
				return i + bit;
			}
		}
	}
	return n;
#else
	const char *p = hay + from;
	const char *end = hay + last_start + 1;
	while (p < end) {
		p = static_cast<const char *>(std::memchr(p, needle[0], static_cast<size_t>(end - p)));
		if (!p) {
			return n;
		}
		if (std::memcmp(p, needle, m) == 0) {
			return static_cast<size_t>(p - hay);
		}
		p++;
	}
	return n;
#endif
}

struct TwoPartLikeMatcher {
	std::string part1;
	std::string part2;
	bool enabled = false;

	explicit TwoPartLikeMatcher(const std::string &pattern) {
		if (pattern.size() < 4 || pattern.front() != '%' || pattern.back() != '%') {
			return;
		}
		const auto first_end = pattern.find('%', 1);
		if (first_end == std::string::npos || first_end == 1) {
			return;
		}
		const auto second_start = first_end + 1;
		const auto second_end = pattern.find('%', second_start);
		if (second_end == std::string::npos || second_end == second_start) {
			return;
		}
		if (pattern.find('%', second_end + 1) != std::string::npos) {
			return;
		}
		part1 = pattern.substr(1, first_end - 1);
		part2 = pattern.substr(second_start, second_end - second_start);
		enabled = !part1.empty() && !part2.empty();
	}

	//! text LIKE '%part1%part2%' (part1 first occurrence is sufficient: any later
	//! part1 occurrence leaves strictly less room for part2).
	bool Matches(const char *data, size_t n) const {
		if (!enabled || n < part1.size() + part2.size()) {
			return false;
		}
		const size_t found = FindFrom(data, n, 0, part1.data(), part1.size());
		if (found == n) {
			return false;
		}
		return FindFrom(data, n, found + part1.size(), part2.data(), part2.size()) != n;
	}
};

} // namespace q13
} // namespace accelerator_codegen
