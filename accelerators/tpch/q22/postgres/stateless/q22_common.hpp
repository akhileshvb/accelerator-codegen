// TPC-H Q22 (stateless PostgreSQL): shared pieces of the single-threaded and
// parallel kernels.
//
// Plan: one pass over customer (phone prefix match -> compact arrays of the
// matching customers plus the exact sum/count of their positive balances), one
// pass over orders (bit vector "customer has orders", SIMD min/max per batch so
// the marking loop needs no bounds checks), then a tiny final pass.
#pragma once

#include "backends/postgres/api/stateless/pg_stateless.hpp"
#include "prims/simd.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace accelerator_codegen::pg_stateless::q22 {

inline constexpr const char *kSqlArgs = "text, text, text, text, text, text, text";
inline constexpr const char *kSqlReturns = "TABLE (cntrycode text, numcust bigint, totacctbal numeric)";

inline constexpr int kMaxCodes = 7;

//! Phone prefix (first two bytes) -> slot + 1 (0 = not wanted).
struct CodeSet {
	int n = 0;
	char codes[kMaxCodes][2] = {};
	uint8_t slot_of[65536] = {};
};

inline void BindCodes(const Args &args, CodeSet &set) {
	for (int i = 0; i < kMaxCodes; i++) {
		const std::string s = args.Text(i);
		if (s.size() != 2) {
			continue; // SUBSTRING(phone, 1, 2) can never equal it
		}
		const unsigned key = static_cast<unsigned char>(s[0]) | (static_cast<unsigned>(static_cast<unsigned char>(s[1])) << 8);
		if (set.slot_of[key] == 0) {
			set.codes[set.n][0] = s[0];
			set.codes[set.n][1] = s[1];
			set.n++;
			set.slot_of[key] = static_cast<uint8_t>(set.n);
		}
	}
}

enum : int { kCustKey = 0, kCustPhone = 1, kCustBal = 2 };

inline ScanSpec CustomerSpec() {
	ScanSpec s;
	s.table = "customer";
	s.columns = {{"c_custkey", ColType::Int64}, {"c_phone", ColType::Text}, {"c_acctbal", ColType::Decimal, 2}};
	// Non-positive balances can neither enter the average nor exceed it (avg > 0 whenever any row counts).
	{FilterSpec f; f.column = kCustBal; f.op = FilterOp::Between; f.lo = 1; f.hi = INT64_MAX; s.filters.push_back(f);}
	return s;
}


inline ScanSpec OrdersSpec() {
	// Probing orders by o_custkey (IntIn on the candidate customers) was measured slower than
	// this full scan: ~20k candidates exceed the bitmap threshold, and chunked bitmap scans cost
	// ~2.8us/key of index work plus ~200k heap fetches (>= the 100 ms seq scan).
	ScanSpec s;
	s.table = "orders";
	s.columns = {{"o_custkey", ColType::Int64}};
	return s;
}

struct Matched {
	std::vector<int64_t> custkey;
	std::vector<int64_t> acctbal;
	std::vector<uint8_t> slot; // index into CodeSet::codes
	__int128 positive_sum = 0;
	int64_t positive_count = 0;
};

inline void ConsumeCustomers(const Batch &b, const CodeSet &set, Matched &out) {
	const int64_t *ACCEL_RESTRICT keys = b.I64(kCustKey);
	const StringRef *ACCEL_RESTRICT phones = b.Str(kCustPhone);
	const int64_t *ACCEL_RESTRICT bals = b.I64(kCustBal);
	const std::size_t n = b.count;
	const std::size_t base = out.custkey.size();
	out.custkey.resize(base + n + 1);
	out.acctbal.resize(base + n + 1);
	out.slot.resize(base + n + 1);
	int64_t *ACCEL_RESTRICT ok = out.custkey.data() + base;
	int64_t *ACCEL_RESTRICT ob = out.acctbal.data() + base;
	uint8_t *ACCEL_RESTRICT os = out.slot.data() + base;
	std::size_t count = 0;
	int64_t psum = 0, pcnt = 0; // |bal| < 10^15, <= 2048 rows per batch: fits int64
	for (std::size_t i = 0; i < n; i++) {
		const char *p = phones[i].ptr;
		const bool long_enough = phones[i].len >= 2;
		const unsigned key = long_enough ? (static_cast<unsigned char>(p[0]) | (static_cast<unsigned>(static_cast<unsigned char>(p[1])) << 8)) : 0;
		const unsigned s = long_enough ? set.slot_of[key] : 0;
		const bool match = s != 0;
		ok[count] = keys[i];
		ob[count] = bals[i];
		os[count] = static_cast<uint8_t>(s - 1);
		const bool pos = match & (bals[i] > 0);
		psum += pos ? bals[i] : 0;
		pcnt += pos ? 1 : 0;
		count += match ? 1 : 0;
	}
	out.custkey.resize(base + count);
	out.acctbal.resize(base + count);
	out.slot.resize(base + count);
	out.positive_sum += psum;
	out.positive_count += pcnt;
}

//! Bit vector over customer keys; keys outside [0, kDenseLimit] go to a vector.
class HasOrders {
public:
	static constexpr int64_t kDenseLimit = int64_t(1) << 28;

	void MarkBatch(const int64_t *ACCEL_RESTRICT keys, std::size_t n) {
		if (n == 0) {
			return;
		}
		int64_t lo = keys[0], hi = keys[0];
		std::size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
		if (n >= 8) {
			__m512i vlo = _mm512_loadu_si512(keys);
			__m512i vhi = vlo;
			for (i = 8; i + 8 <= n; i += 8) {
				const __m512i v = _mm512_loadu_si512(keys + i);
				vlo = _mm512_min_epi64(vlo, v);
				vhi = _mm512_max_epi64(vhi, v);
			}
			lo = _mm512_reduce_min_epi64(vlo);
			hi = _mm512_reduce_max_epi64(vhi);
		}
#endif
		for (; i < n; i++) {
			lo = keys[i] < lo ? keys[i] : lo;
			hi = keys[i] > hi ? keys[i] : hi;
		}
		if (lo < 0 || hi > kDenseLimit) {
			for (std::size_t j = 0; j < n; j++) {
				MarkOne(keys[j]);
			}
			return;
		}
		const std::size_t need = (static_cast<std::size_t>(hi) >> 6) + 1;
		if (need > words.size()) {
			words.resize(std::max(need, words.size() * 2), 0);
		}
		uint64_t *ACCEL_RESTRICT w = words.data();
		for (std::size_t j = 0; j < n; j++) {
			const uint64_t k = static_cast<uint64_t>(keys[j]);
			w[k >> 6] |= uint64_t(1) << (k & 63);
		}
	}

	void MarkOne(int64_t k) {
		if (k < 0 || k > kDenseLimit) {
			overflow.push_back(k);
			return;
		}
		const std::size_t w = static_cast<std::size_t>(k) >> 6;
		if (w >= words.size()) {
			words.resize(w + 1, 0);
		}
		words[w] |= uint64_t(1) << (k & 63);
	}

	void Finish() {
		std::sort(overflow.begin(), overflow.end());
		overflow.erase(std::unique(overflow.begin(), overflow.end()), overflow.end());
	}

	bool Has(int64_t k) const {
		if (k < 0 || k > kDenseLimit) {
			return std::binary_search(overflow.begin(), overflow.end(), k);
		}
		const std::size_t w = static_cast<std::size_t>(k) >> 6;
		return w < words.size() && ((words[w] >> (k & 63)) & 1);
	}

	void Merge(const HasOrders &o) {
		if (o.words.size() > words.size()) {
			words.resize(o.words.size(), 0);
		}
		for (std::size_t i = 0; i < o.words.size(); i++) {
			words[i] |= o.words[i];
		}
		overflow.insert(overflow.end(), o.overflow.begin(), o.overflow.end());
	}

	std::vector<uint64_t> words;
	std::vector<int64_t> overflow;
};

//! Per-code partial result.
struct Totals {
	int64_t counts[kMaxCodes] = {};
	__int128 sums[kMaxCodes] = {};
};

//! Candidates (balance above the average) of a Matched set: keys + slots + balances.
inline void Candidates(const Matched &m, __int128 psum, int64_t pcnt, std::vector<int64_t> &keys,
                       std::vector<int64_t> &bals, std::vector<uint8_t> &slots) {
	if (pcnt <= 0) {
		return;
	}
	const __int128 cnt = pcnt;
	for (std::size_t i = 0; i < m.custkey.size(); i++) {
		if (static_cast<__int128>(m.acctbal[i]) * cnt > psum) {
			keys.push_back(m.custkey[i]);
			bals.push_back(m.acctbal[i]);
			slots.push_back(m.slot[i]);
		}
	}
}

//! Anti-join the candidates against orders (one index probe per key) and add to totals.
inline void AntiJoin(const std::vector<int64_t> &keys, const std::vector<int64_t> &bals,
                     const std::vector<uint8_t> &slots, Totals &t) {
	if (keys.empty()) {
		return;
	}
	const std::vector<uint8_t> exists = KeysExist("orders", "o_custkey", keys);
	for (std::size_t i = 0; i < keys.size(); i++) {
		if (!exists[i]) {
			t.counts[slots[i]]++;
			t.sums[slots[i]] += bals[i];
		}
	}
}

inline void Emit(const CodeSet &set, const Totals &t, ResultWriter &out) {
	const int64_t *counts = t.counts;
	const __int128 *totals = t.sums;
int order[kMaxCodes];
	for (int i = 0; i < set.n; i++) {
		order[i] = i;
	}
	std::sort(order, order + set.n, [&](int a, int b) {
		return std::string_view(set.codes[a], 2) < std::string_view(set.codes[b], 2);
	});
	for (int k = 0; k < set.n; k++) {
		const int s = order[k];
		if (counts[s] == 0) {
			continue;
		}
		out.Text(std::string_view(set.codes[s], 2));
		out.Int(counts[s]);
		out.Decimal(totals[s], 2);
		out.EndRow();
	}
}

} // namespace accelerator_codegen::pg_stateless::q22
