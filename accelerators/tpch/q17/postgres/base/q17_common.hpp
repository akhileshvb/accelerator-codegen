// TPC-H Q17 (base PostgreSQL): shared pieces of the single-threaded and
// parallel kernels.
//
// Plan: scan part with the brand/container filters pushed into the scan ->
// bit vector of qualifying part keys.  One lineitem pass: an AVX-512 gather on
// the bit vector selects the (rare, ~0.03%) rows of qualifying parts, whose
// (partkey, quantity, extendedprice) are collected and also summed per part
// (exact int64 sum + count, i.e. avg over ALL lineitems of the part, which
// are all seen because every row of a qualifying part is collected).  The
// final pass over the collected rows keeps quantity < 0.2*avg, compared
// exactly as qty*5*cnt < sum(qty).
#pragma once

#include "backends/postgres/api/base/pg_base.hpp"
#include "prims/simd.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace accelerator_codegen::pg_base::q17 {

inline constexpr const char *kSqlArgs = "text, text";
inline constexpr const char *kSqlReturns = "TABLE (avg_yearly double precision)";

//! Part keys up to this value live in the bit vector, larger ones in a sorted vector.
inline constexpr int64_t kDenseLimit = int64_t(1) << 24;

inline ScanSpec PartSpec(const std::string &brand, const std::string &container) {
	ScanSpec s;
	s.table = "part";
	s.columns = {{"p_partkey", ColType::Int64}, {"p_brand", ColType::Text}, {"p_container", ColType::Text}};
	s.filters = {{1, FilterOp::TextEq, 0, 0, {brand}}, {2, FilterOp::TextEq, 0, 0, {container}}};
	return s;
}

inline ScanSpec LineitemSpec() {
	ScanSpec s;
	s.table = "lineitem";
	s.columns = {{"l_partkey", ColType::Int64},
	             {"l_quantity", ColType::Decimal, 2},
	             {"l_extendedprice", ColType::Decimal, 2}};
	return s;
}

//! Qualifying part keys: bit vector for keys <= kDenseLimit, sorted vector for the rest.
struct PartSet {
	std::vector<uint64_t> words;
	std::vector<int64_t> big; // sorted, unique
	int64_t max_dense = -1;
	int64_t count = 0;

	void Consume(const Batch &b) {
		const int64_t *k = b.I64(0);
		for (std::size_t i = 0; i < b.count; i++) {
			count++;
			if (k[i] < 0 || k[i] > kDenseLimit) {
				big.push_back(k[i]);
				continue;
			}
			const std::size_t w = static_cast<std::size_t>(k[i]) >> 6;
			if (w >= words.size()) {
				words.resize(std::max(w + 1, words.size() * 2), 0);
			}
			words[w] |= uint64_t(1) << (k[i] & 63);
			max_dense = std::max(max_dense, k[i]);
		}
	}
	void Finish() {
		if (max_dense < 0) {
			max_dense = 0;
			words.assign(1, 0);
		}
		std::sort(big.begin(), big.end());
		big.erase(std::unique(big.begin(), big.end()), big.end());
	}
};

struct Row {
	int64_t key, qty, price;
};

//! Per-participant lineitem state.
struct Collector {
	const uint64_t *words;
	int64_t max_dense;
	const int64_t *big;
	std::size_t nbig;
	std::vector<Row> rows;

	explicit Collector(const PartSet &p) : words(p.words.data()), max_dense(p.max_dense), big(p.big.data()), nbig(p.big.size()) {
	}
	Collector(const uint64_t *w, int64_t md, const int64_t *bg, std::size_t nb)
	    : words(w), max_dense(md), big(bg), nbig(nb) {
	}

	bool InBig(int64_t k) const {
		return nbig && std::binary_search(big, big + nbig, k);
	}

	void Consume(const Batch &b) {
		const int64_t *ACCEL_RESTRICT key = b.I64(0);
		const int64_t *ACCEL_RESTRICT qty = b.I64(1);
		const int64_t *ACCEL_RESTRICT price = b.I64(2);
		const std::size_t n = b.count;
		std::size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
		const __m512i vmax = _mm512_set1_epi64(max_dense);
		const __m512i one = _mm512_set1_epi64(1);
		const __m512i m63 = _mm512_set1_epi64(63);
		for (; i + 8 <= n; i += 8) {
			const __m512i k = _mm512_loadu_si512(key + i);
			const __mmask8 inr = _mm512_cmple_epu64_mask(k, vmax);
			const __m512i w = _mm512_mask_i64gather_epi64(_mm512_setzero_si512(), inr, _mm512_srli_epi64(k, 6),
			                                              words, 8);
			const __m512i bit = _mm512_and_si512(_mm512_srlv_epi64(w, _mm512_and_si512(k, m63)), one);
			unsigned m = _mm512_mask_test_epi64_mask(inr, bit, one);
			if (nbig) {
				m |= static_cast<unsigned>(static_cast<__mmask8>(~inr));
			}
			while (m) {
				const unsigned j = static_cast<unsigned>(__builtin_ctz(m));
				m &= m - 1;
				if ((inr >> j & 1) || InBig(key[i + j])) {
					rows.push_back({key[i + j], qty[i + j], price[i + j]});
				}
			}
		}
#endif
		for (; i < n; i++) {
			const uint64_t k = static_cast<uint64_t>(key[i]);
			const bool hit = k <= static_cast<uint64_t>(max_dense) ? ((words[k >> 6] >> (k & 63)) & 1) : InBig(key[i]);
			if (hit) {
				rows.push_back({key[i], qty[i], price[i]});
			}
		}
	}
};

//! Merge collected rows of all participants, evaluate, emit.  Rows are grouped
//! by part key after sorting, so the result does not depend on participant order.
inline void Finish(std::vector<Row> &rows, ResultWriter &out) {
	std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.key < b.key; });
	__int128 total = 0;
	bool any = false;
	for (std::size_t lo = 0; lo < rows.size();) {
		std::size_t hi = lo;
		int64_t sum = 0;
		while (hi < rows.size() && rows[hi].key == rows[lo].key) {
			sum += rows[hi].qty;
			hi++;
		}
		const int64_t cnt = static_cast<int64_t>(hi - lo);
		for (std::size_t j = lo; j < hi; j++) {
			// qty < 0.2 * (sum/cnt)  <=>  5 * qty * cnt < sum
			if (static_cast<__int128>(rows[j].qty) * 5 * cnt < sum) {
				total += rows[j].price;
				any = true;
			}
		}
		lo = hi;
	}
	if (!any) {
		out.Null();
	} else {
		out.Double(static_cast<double>(total) / 700.0); // SUM (scale 2) / 7.0
	}
	out.EndRow();
}

} // namespace accelerator_codegen::pg_base::q17
