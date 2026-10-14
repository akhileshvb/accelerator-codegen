// TPC-H Q18 (stateless PostgreSQL): parameters, scan specs and kernels shared
// by the single-threaded and parallel versions.
//
// Plan: (1) lineitem scan -> SUM(l_quantity) per orderkey.  Lineitems of an
// order are physically adjacent, so each batch is reduced to runs of equal
// keys first (AVX-512 boundary detection over a prefix-sum array) and only the
// runs are added to an open-addressing table (prefetched).  (2) The orders
// with sum > threshold (few) are looked up with an orders scan (orderkey range
// pushed into the scan) and (3) their customers with a customer scan
// (custkey range pushed into the scan).  Sorted by totalprice DESC, orderdate,
// orderkey.
#pragma once

#include "backends/postgres/api/stateless/pg_stateless.hpp"
#include "prims/simd.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace accelerator_codegen::pg_stateless::q18 {

inline constexpr const char *kSqlArgs = "numeric";
inline constexpr const char *kSqlReturns =
    "TABLE (c_name text, c_custkey bigint, o_orderkey bigint, o_orderdate date, o_totalprice numeric, "
    "\"sum(l_quantity)\" numeric)";

enum : int { kLOrderkey = 0, kLQuantity = 1 };
enum : int { kOOrderkey = 0, kOCustkey = 1, kODate = 2, kOTotalprice = 3 };
enum : int { kCCustkey = 0, kCName = 1 };

inline int64_t BindThreshold(const Args &args) {
	return args.Decimal(0, 2); // quantity scale 2; HAVING sum > threshold
}

inline ScanSpec LineitemSpec() {
	ScanSpec s;
	s.table = "lineitem";
	s.columns = {{"l_orderkey", ColType::Int64}, {"l_quantity", ColType::Decimal, 2}};
	return s;
}

inline ScanSpec OrdersSpec(int64_t kmin, int64_t kmax) {
	ScanSpec s;
	s.table = "orders";
	s.columns = {{"o_orderkey", ColType::Int64},
	             {"o_custkey", ColType::Int64},
	             {"o_orderdate", ColType::Date},
	             {"o_totalprice", ColType::Decimal, 2}};
	s.filters = {{kOOrderkey, FilterOp::Between, kmin, kmax}};
	return s;
}

inline ScanSpec CustomerSpec(int64_t kmin, int64_t kmax) {
	ScanSpec s;
	s.table = "customer";
	s.columns = {{"c_custkey", ColType::Int64}, {"c_name", ColType::Text}};
	s.filters = {{kCCustkey, FilterOp::Between, kmin, kmax}};
	return s;
}

//! orderkey -> running sum; keys/sums are kept in first-seen order (which is
//! nearly key order for a clustered lineitem) next to an open-addressing index.
class SumTable {
public:
	SumTable() {
		Rehash(1 << 12);
	}
	void EnsureRoom(std::size_t extra) {
		if ((keys_.size() + extra) * 2 > slots_.size()) {
			std::size_t cap = slots_.size();
			while ((keys_.size() + extra) * 2 > cap) {
				cap <<= 1;
			}
			Rehash(cap);
		}
	}
	std::size_t Home(int64_t key) const {
		return (static_cast<uint64_t>(key) * 0x9E3779B97F4A7C15ull) >> shift_;
	}
	const void *Addr(int64_t key) const {
		return &slots_[Home(key)];
	}
	//! Requires EnsureRoom() for all additions since the last call.
	void Add(int64_t key, int64_t sum) {
		std::size_t s = Home(key);
		for (;;) {
			Slot &sl = slots_[s];
			if (sl.idx == kEmpty) {
				sl.key = key;
				sl.idx = static_cast<uint32_t>(keys_.size());
				keys_.push_back(key);
				sums_.push_back(sum);
				return;
			}
			if (sl.key == key) {
				sums_[sl.idx] += sum;
				return;
			}
			s = (s + 1) & mask_;
		}
	}
	//! Add n (key, sum) pairs with a prefetch pipeline.
	void AddMany(const int64_t *keys, const int64_t *sums, std::size_t n) {
		EnsureRoom(n);
		constexpr std::size_t kDist = 8;
		for (std::size_t j = 0; j < std::min(n, kDist); j++) {
			ACCEL_PREFETCH(Addr(keys[j]));
		}
		for (std::size_t j = 0; j < n; j++) {
			if (j + kDist < n) {
				ACCEL_PREFETCH(Addr(keys[j + kDist]));
			}
			Add(keys[j], sums[j]);
		}
	}
	const std::vector<int64_t> &Keys() const {
		return keys_;
	}
	const std::vector<int64_t> &Sums() const {
		return sums_;
	}

private:
	static constexpr uint32_t kEmpty = 0xFFFFFFFFu;
	struct Slot {
		int64_t key;
		uint32_t idx;
		uint32_t pad;
	};
	void Rehash(std::size_t cap) {
		slots_.assign(cap, Slot {0, kEmpty, 0});
		shift_ = 64 - __builtin_ctzll(cap);
		mask_ = cap - 1;
		for (std::size_t i = 0; i < keys_.size(); i++) {
			std::size_t s = Home(keys_[i]);
			while (slots_[s].idx != kEmpty) {
				s = (s + 1) & mask_;
			}
			slots_[s] = Slot {keys_[i], static_cast<uint32_t>(i), 0};
		}
	}
	std::vector<Slot> slots_;
	std::vector<int64_t> keys_;
	std::vector<int64_t> sums_;
	int shift_ = 52;
	std::size_t mask_ = 0;
};

//! Reduce each batch to (orderkey, sum) runs of adjacent equal keys.
struct RunReducer {
	std::vector<int64_t> cum, rk, rs;
	void Consume(const Batch &b, SumTable &table) {
		const std::size_t n = b.count;
		if (n == 0) {
			return;
		}
		const int64_t *ACCEL_RESTRICT key = b.I64(kLOrderkey);
		const int64_t *ACCEL_RESTRICT qty = b.I64(kLQuantity);
		if (cum.size() < n) {
			cum.resize(n);
			rk.resize(n);
			rs.resize(n);
		}
		int64_t acc = 0;
		for (std::size_t i = 0; i < n; i++) {
			acc += qty[i];
			cum[i] = acc;
		}
		std::size_t m = 0, start = 0;
		int64_t base = 0;
		auto run_end = [&](std::size_t e) {
			rk[m] = key[e];
			rs[m] = cum[e] - base;
			base = cum[e];
			start = e + 1;
			m++;
		};
		std::size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
		for (; i + 9 <= n; i += 8) {
			unsigned mask = _mm512_cmpneq_epi64_mask(_mm512_loadu_si512(key + i), _mm512_loadu_si512(key + i + 1));
			while (mask) {
				run_end(i + static_cast<std::size_t>(__builtin_ctz(mask)));
				mask &= mask - 1;
			}
		}
#endif
		for (; i + 1 < n; i++) {
			if (key[i] != key[i + 1]) {
				run_end(i);
			}
		}
		run_end(n - 1);
		(void)start;
		table.AddMany(rk.data(), rs.data(), m);
	}
};

//! Orders whose total quantity exceeds the threshold: keys sorted, sums aligned.
struct Qual {
	std::vector<int64_t> keys, sums;
};

inline Qual Qualifying(const SumTable &t, int64_t threshold) {
	std::vector<std::pair<int64_t, int64_t>> v;
	const auto &k = t.Keys();
	const auto &s = t.Sums();
	for (std::size_t i = 0; i < k.size(); i++) {
		if (s[i] > threshold) {
			v.emplace_back(k[i], s[i]);
		}
	}
	std::sort(v.begin(), v.end());
	Qual q;
	for (const auto &e : v) {
		q.keys.push_back(e.first);
		q.sums.push_back(e.second);
	}
	return q;
}

//! Compact (de)serialization of a SumTable: zigzag-varint key deltas (keys are
//! nearly ascending in first-seen order) and varint sums.
inline void PutVarint(std::string &buf, uint64_t v) {
	while (v >= 0x80) {
		buf.push_back(static_cast<char>(v | 0x80));
		v >>= 7;
	}
	buf.push_back(static_cast<char>(v));
}

inline std::string EncodeTable(const SumTable &t) {
	std::string buf;
	const auto &k = t.Keys();
	const auto &s = t.Sums();
	buf.reserve(k.size() * 3 + 16);
	PutVarint(buf, k.size());
	int64_t prev = 0;
	for (std::size_t i = 0; i < k.size(); i++) {
		const int64_t d = k[i] - prev;
		prev = k[i];
		PutVarint(buf, (static_cast<uint64_t>(d) << 1) ^ static_cast<uint64_t>(d >> 63));
		PutVarint(buf, (static_cast<uint64_t>(s[i]) << 1) ^ static_cast<uint64_t>(s[i] >> 63));
	}
	return buf;
}

inline void DecodeInto(std::string_view buf, SumTable &t) {
	const unsigned char *p = reinterpret_cast<const unsigned char *>(buf.data());
	const unsigned char *end = p + buf.size();
	auto get = [&]() {
		uint64_t v = 0;
		int shift = 0;
		for (;;) {
			if (p >= end) {
				throw KernelError("q18: truncated partial");
			}
			const unsigned char c = *p++;
			v |= static_cast<uint64_t>(c & 0x7F) << shift;
			if (!(c & 0x80)) {
				return v;
			}
			shift += 7;
		}
	};
	const uint64_t n = get();
	constexpr std::size_t kChunk = 1024;
	int64_t keys[kChunk], sums[kChunk];
	int64_t prev = 0;
	for (uint64_t done = 0; done < n;) {
		const std::size_t m = static_cast<std::size_t>(std::min<uint64_t>(kChunk, n - done));
		for (std::size_t i = 0; i < m; i++) {
			const uint64_t zd = get();
			prev += static_cast<int64_t>(zd >> 1) ^ -static_cast<int64_t>(zd & 1);
			keys[i] = prev;
			const uint64_t zs = get();
			sums[i] = static_cast<int64_t>(zs >> 1) ^ -static_cast<int64_t>(zs & 1);
		}
		t.AddMany(keys, sums, m);
		done += m;
	}
}

struct OrderRow {
	int64_t orderkey;
	int64_t custkey;
	int64_t totalprice;
	int64_t sum;
	int32_t date;
	int32_t pad;
};

//! Probe a batch of the orders scan against the sorted qualifying keys.
inline void CollectOrders(const Batch &b, const std::vector<int64_t> &keys, std::vector<OrderRow> &out) {
	const int64_t *ok = b.I64(kOOrderkey);
	const int64_t *ck = b.I64(kOCustkey);
	const int32_t *dt = b.I32(kODate);
	const int64_t *tp = b.I64(kOTotalprice);
	for (std::size_t i = 0; i < b.count; i++) {
		if (std::binary_search(keys.begin(), keys.end(), ok[i])) {
			out.push_back(OrderRow {ok[i], ck[i], tp[i], 0, dt[i], 0});
		}
	}
}

//! Fill sums from the (key-sorted) qualifying list and emit joined rows.
inline void Finish(std::vector<OrderRow> &rows, const Qual &qual, ResultWriter &out) {
	const std::vector<int64_t> &qual_keys = qual.keys;
	const std::vector<int64_t> &qual_sums = qual.sums;
	if (rows.empty()) {
		return;
	}
	for (auto &r : rows) {
		const auto it = std::lower_bound(qual_keys.begin(), qual_keys.end(), r.orderkey);
		r.sum = qual_sums[static_cast<std::size_t>(it - qual_keys.begin())];
	}
	int64_t cmin = rows[0].custkey, cmax = rows[0].custkey;
	for (const auto &r : rows) {
		cmin = std::min(cmin, r.custkey);
		cmax = std::max(cmax, r.custkey);
	}
	std::vector<int64_t> need;
	for (const auto &r : rows) {
		need.push_back(r.custkey);
	}
	std::sort(need.begin(), need.end());
	need.erase(std::unique(need.begin(), need.end()), need.end());
	std::vector<std::string> names(need.size());
	std::vector<uint8_t> have(need.size(), 0);
	ScanTable(CustomerSpec(cmin, cmax), [&](const Batch &b) {
		const int64_t *ck = b.I64(kCCustkey);
		const StringRef *nm = b.Str(kCName);
		for (std::size_t i = 0; i < b.count; i++) {
			const auto it = std::lower_bound(need.begin(), need.end(), ck[i]);
			if (it != need.end() && *it == ck[i]) {
				const std::size_t j = static_cast<std::size_t>(it - need.begin());
				names[j] = std::string(nm[i].View());
				have[j] = 1;
			}
		}
	});
	std::sort(rows.begin(), rows.end(), [](const OrderRow &a, const OrderRow &b) {
		if (a.totalprice != b.totalprice) {
			return a.totalprice > b.totalprice;
		}
		if (a.date != b.date) {
			return a.date < b.date;
		}
		return a.orderkey < b.orderkey;
	});
	for (const auto &r : rows) {
		const std::size_t j =
		    static_cast<std::size_t>(std::lower_bound(need.begin(), need.end(), r.custkey) - need.begin());
		if (!have[j]) {
			continue; // inner join: customer must exist
		}
		out.Text(names[j]);
		out.Int(r.custkey);
		out.Int(r.orderkey);
		out.Date(r.date);
		out.Decimal(r.totalprice, 2);
		out.Decimal(r.sum, 2);
		out.EndRow();
	}
}

} // namespace accelerator_codegen::pg_stateless::q18
