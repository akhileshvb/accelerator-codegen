#include "sec_edgar/q1/duckdb/aux/adsh_key.hpp"

#include "duckdb/common/types/string_type.hpp"

#include <cstring>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define Q1_HAVE_X86_SIMD 1
#include <immintrin.h>
#endif

namespace accelerator_codegen::aux_state::query::q1 {
namespace {

constexpr std::size_t kAdshLen = 20;

} // namespace

bool ParseAdshKeyScalar(const char *s, std::size_t len, uint64_t &key) {
	if (len != kAdshLen || s[10] != '-' || s[13] != '-') {
		return false;
	}
	uint64_t value = 0;
	for (std::size_t i = 0; i < kAdshLen; ++i) {
		if (i == 10 || i == 13) {
			continue;
		}
		const unsigned digit = static_cast<unsigned char>(s[i]) - static_cast<unsigned>('0');
		if (digit > 9) {
			return false;
		}
		value = value * 10 + digit;
	}
	key = value;
	return true;
}

#ifdef Q1_HAVE_X86_SIMD
namespace {

// Digits D0..D17 = s[0..9], s[11..12], s[14..19]. The first sixteen are parsed
// with pmaddubsw/pmaddwd (8 -> 4 -> 2 -> 1 digit-group tree); the last two are
// scalar.
__attribute__((target("ssse3,sse4.1"))) inline bool ParseAdshKeySse(const char *s, uint64_t &key) {
	const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(s));     // s[0..15]
	const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i *>(s + 4)); // s[4..19]
	// dashes at 10 and 13
	const __m128i dash = _mm_set1_epi8('-');
	const int dash_mask = _mm_movemask_epi8(_mm_cmpeq_epi8(a, dash));
	if ((dash_mask & 0x2400) != 0x2400) {
		return false;
	}
	// lanes 0..13 <- s[0..9], s[11], s[12], s[14], s[15]
	const __m128i mask_a = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 14, 15, -128, -128);
	// lanes 14,15 <- s[16], s[17] = c[12], c[13]
	const __m128i mask_c = _mm_setr_epi8(-128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
	                                     -128, -128, 12, 13);
	__m128i v = _mm_or_si128(_mm_shuffle_epi8(a, mask_a), _mm_shuffle_epi8(c, mask_c));
	v = _mm_sub_epi8(v, _mm_set1_epi8('0'));
	const __m128i nine = _mm_set1_epi8(9);
	if (_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_min_epu8(v, nine), v)) != 0xFFFF) {
		return false;
	}
	const unsigned t0 = static_cast<unsigned char>(s[18]) - static_cast<unsigned>('0');
	const unsigned t1 = static_cast<unsigned char>(s[19]) - static_cast<unsigned>('0');
	if (t0 > 9 || t1 > 9) {
		return false;
	}
	const __m128i m1 = _mm_maddubs_epi16(v, _mm_setr_epi8(10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1));
	const __m128i m2 = _mm_madd_epi16(m1, _mm_setr_epi16(100, 1, 100, 1, 100, 1, 100, 1));
	const __m128i m3 = _mm_packus_epi32(m2, m2);
	const __m128i m4 = _mm_madd_epi16(m3, _mm_setr_epi16(10000, 1, 10000, 1, 10000, 1, 10000, 1));
	const uint64_t hi = static_cast<uint32_t>(_mm_cvtsi128_si32(m4));
	const uint64_t lo = static_cast<uint32_t>(_mm_extract_epi32(m4, 1));
	key = (hi * 100000000ULL + lo) * 100ULL + t0 * 10 + t1;
	return true;
}

__attribute__((target("ssse3,sse4.1"))) void ParseBatchSse(const duckdb::string_t *values, const uint32_t *sel,
                                                            std::size_t count, uint64_t *out) {
	constexpr std::size_t kPrefetchDistance = 16;
	for (std::size_t i = 0; i < count; ++i) {
		if (i + kPrefetchDistance < count) {
			const auto &ahead = values[sel ? sel[i + kPrefetchDistance] : i + kPrefetchDistance];
			if (!ahead.IsInlined()) {
				__builtin_prefetch(ahead.GetData(), 0, 1);
			}
		}
		const auto &value = values[sel ? sel[i] : i];
		uint64_t key = 0;
		if (value.GetSize() == kAdshLen && ParseAdshKeySse(value.GetData(), key)) {
			out[i] = key;
		} else {
			out[i] = kInvalidAdshKey;
		}
	}
}

bool DetectSimd() {
	__builtin_cpu_init();
	return __builtin_cpu_supports("ssse3") && __builtin_cpu_supports("sse4.1");
}

} // namespace
#endif

bool AdshKeySimdAvailable() {
#ifdef Q1_HAVE_X86_SIMD
	static const bool available = DetectSimd();
	return available;
#else
	return false;
#endif
}

void ParseAdshKeyBatchScalar(const duckdb::string_t *values, const uint32_t *sel, std::size_t count,
                             uint64_t *out_keys) {
	for (std::size_t i = 0; i < count; ++i) {
		const auto &value = values[sel ? sel[i] : i];
		uint64_t key = 0;
		out_keys[i] = ParseAdshKeyScalar(value.GetData(), value.GetSize(), key) ? key : kInvalidAdshKey;
	}
}

void ParseAdshKeyBatch(const duckdb::string_t *values, const uint32_t *sel, std::size_t count, uint64_t *out_keys) {
#ifdef Q1_HAVE_X86_SIMD
	if (AdshKeySimdAvailable()) {
		ParseBatchSse(values, sel, count, out_keys);
		return;
	}
#endif
	ParseAdshKeyBatchScalar(values, sel, count, out_keys);
}

bool ParseAdshKey(const char *s, std::size_t len, uint64_t &key) {
#ifdef Q1_HAVE_X86_SIMD
	if (len == kAdshLen && AdshKeySimdAvailable()) {
		return ParseAdshKeySse(s, key);
	}
#endif
	return ParseAdshKeyScalar(s, len, key);
}

} // namespace accelerator_codegen::aux_state::query::q1
