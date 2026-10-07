// TPC-H Q13 (stateless PostgreSQL): shared pieces of the single-threaded and
// parallel kernels.
//
// customer LEFT JOIN orders ON c_custkey = o_custkey AND o_comment NOT LIKE p:
// per-customer order counts live in a dense uint32 array indexed by custkey
// (orders whose customer does not exist are ignored); the "%a%b%" pattern is
// evaluated with an AVX-512BW substring search (like2.hpp idea), any other
// pattern with a generic LIKE matcher.
#pragma once

#include "accelerator_codegen/runtime/postgres_stateless/pg_stateless.hpp"
#include "accelerator_codegen/runtime/simd.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace accelerator_codegen::pg_stateless::q13 {

inline constexpr const char *kSqlArgs = "text";
inline constexpr const char *kSqlReturns = "TABLE (c_count bigint, custdist bigint)";

inline constexpr int64_t kDenseLimit = int64_t(1) << 28;

#if defined(__AVX512BW__) && defined(__AVX512F__) && defined(__AVX512VL__)
#define Q13PG_SIMD_STRSTR 1
#endif

//! First occurrence of needle[0..m) in hay[from..n), or n if none (m >= 1).
inline std::size_t FindFrom(const char *hay, std::size_t n, std::size_t from, const char *needle, std::size_t m) {
	if (n < m || from > n - m) {
		return n;
	}
	const std::size_t last_start = n - m;
#if defined(Q13PG_SIMD_STRSTR)
	const __m512i first = _mm512_set1_epi8(needle[0]);
	const __m512i last = _mm512_set1_epi8(needle[m - 1]);
	for (std::size_t i = from; i <= last_start; i += 64) {
		const std::size_t positions = last_start - i + 1;
		const std::size_t npos = positions < 64 ? positions : 64;
		const __mmask64 valid = npos == 64 ? ~0ULL : ((1ULL << npos) - 1);
		const std::size_t b_avail = n - (i + m - 1);
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
		p = static_cast<const char *>(std::memchr(p, needle[0], static_cast<std::size_t>(end - p)));
		if (!p) {
			return n;
		}
		if (std::memcmp(p, needle, m) == 0) {
			return static_cast<std::size_t>(p - hay);
		}
		p++;
	}
	return n;
#endif
}

//! Generic SQL LIKE (backslash escape, '%' and '_' wildcards, byte semantics).
inline bool LikeGeneric(const char *t, std::size_t tn, const char *p, std::size_t pn) {
	std::size_t ti = 0, pi = 0;
	std::size_t star_p = std::string::npos, star_t = 0;
	while (ti < tn) {
		if (pi < pn && p[pi] == '%') {
			star_p = ++pi;
			star_t = ti;
			continue;
		}
		bool ok = false;
		std::size_t adv = 1;
		if (pi < pn) {
			if (p[pi] == '_') {
				ok = true;
			} else if (p[pi] == '\\' && pi + 1 < pn) {
				ok = t[ti] == p[pi + 1];
				adv = 2;
			} else {
				ok = t[ti] == p[pi];
			}
		}
		if (ok) {
			ti++;
			pi += adv;
		} else if (star_p != std::string::npos) {
			pi = star_p;
			ti = ++star_t;
		} else {
			return false;
		}
	}
	while (pi < pn && p[pi] == '%') {
		pi++;
	}
	return pi == pn;
}

struct Matcher {
	std::string pattern;
	std::string part1, part2;
	bool two_part = false;

	explicit Matcher(std::string pat) : pattern(std::move(pat)) {
		const std::string &s = pattern;
		if (s.size() < 4 || s.front() != '%' || s.back() != '%') {
			return;
		}
		const auto e1 = s.find('%', 1);
		if (e1 == std::string::npos || e1 == 1) {
			return;
		}
		const auto e2 = s.find('%', e1 + 1);
		if (e2 == std::string::npos || e2 == e1 + 1 || s.find('%', e2 + 1) != std::string::npos) {
			return;
		}
		part1 = s.substr(1, e1 - 1);
		part2 = s.substr(e1 + 1, e2 - e1 - 1);
		if (part1.find_first_of("_\\") != std::string::npos || part2.find_first_of("_\\") != std::string::npos) {
			return;
		}
		two_part = true;
	}

	bool Matches(const char *d, std::size_t n) const {
		if (two_part) {
			if (n < part1.size() + part2.size()) {
				return false;
			}
			const std::size_t f = FindFrom(d, n, 0, part1.data(), part1.size());
			return f != n && FindFrom(d, n, f + part1.size(), part2.data(), part2.size()) != n;
		}
		return LikeGeneric(d, n, pattern.data(), pattern.size());
	}
};

enum : int { kOCust = 0, kOComment = 1 };

inline ScanSpec CustomerSpec() {
	ScanSpec s;
	s.table = "customer";
	s.columns = {{"c_custkey", ColType::Int64}};
	return s;
}

inline ScanSpec OrdersSpec() {
	ScanSpec s;
	s.table = "orders";
	s.columns = {{"o_custkey", ColType::Int64}, {"o_comment", ColType::Text}};
	return s;
}

//! Customer keys (all customers, unique) and the largest key.
struct Customers {
	std::vector<int64_t> keys;
	int64_t max_key = -1;

	void Consume(const Batch &b) {
		const int64_t *k = b.I64(0);
		keys.insert(keys.end(), k, k + b.count);
		for (std::size_t i = 0; i < b.count; i++) {
			max_key = std::max(max_key, k[i]);
		}
	}
	void Check() const {
		if (max_key > kDenseLimit) {
			throw KernelError("Q13: customer keys too large for the dense counter array");
		}
	}
};

//! Count qualifying (NOT LIKE) orders per customer key in [0, max_key].
inline void CountOrders(const Batch &b, const Matcher &m, int64_t max_key, uint32_t *ACCEL_RESTRICT counts) {
	const int64_t *ACCEL_RESTRICT ck = b.I64(kOCust);
	const StringRef *ACCEL_RESTRICT cm = b.Str(kOComment);
	for (std::size_t i = 0; i < b.count; i++) {
		if (i + 8 < b.count) {
			ACCEL_PREFETCH(cm[i + 8].ptr);
		}
		const bool in_range = static_cast<uint64_t>(ck[i]) <= static_cast<uint64_t>(max_key);
		if (in_range && !m.Matches(cm[i].ptr, cm[i].len)) {
			counts[ck[i]]++;
		}
	}
}

inline void Emit(const Customers &c, const uint32_t *counts, ResultWriter &out) {
	std::vector<int64_t> hist;
	for (int64_t k : c.keys) {
		const std::size_t cnt = counts[k];
		if (cnt >= hist.size()) {
			hist.resize(cnt + 1, 0);
		}
		hist[cnt]++;
	}
	struct Row {
		int64_t c_count, custdist;
	};
	std::vector<Row> rows;
	for (std::size_t i = 0; i < hist.size(); i++) {
		if (hist[i]) {
			rows.push_back({static_cast<int64_t>(i), hist[i]});
		}
	}
	std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
		return a.custdist != b.custdist ? a.custdist > b.custdist : a.c_count > b.c_count;
	});
	for (const Row &r : rows) {
		out.Int(r.c_count);
		out.Int(r.custdist);
		out.EndRow();
	}
}

} // namespace accelerator_codegen::pg_stateless::q13
