// TPC-H Q10 (base PostgreSQL): parameters, scan specs and the kernel
// pieces shared by the single-threaded and parallel kernels.
//
// Plan: orders (3-month window, scan filter) -> open-addressing hash
// orderkey -> compact customer id; lineitem (l_returnflag = 'R' pushed into
// the scan) probes the hash (hash + prefetch pass, then probe pass) and sums
// revenue per compact customer id in int64 slots; the top 20 are selected from
// the dense revenue array and only those 20 customers' strings are
// materialized from the customer scan.
#pragma once

#include "backends/postgres/api/base/pg_base.hpp"
#include "prims/simd.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace accelerator_codegen::pg_base::q10 {

inline constexpr const char *kSqlArgs = "date";
inline constexpr const char *kSqlReturns = "TABLE (c_custkey bigint, c_name text, revenue numeric, "
                                           "c_acctbal numeric, n_name text, c_address text, c_phone text, "
                                           "c_comment text)";
inline constexpr std::size_t kLimit = 20;

struct Params {
	int32_t start;
	int32_t end_excl;
};

inline Params Bind(const Args &args) {
	Params p;
	p.start = args.Date(0);
	int y;
	unsigned m, d;
	CivilFromDays(p.start, y, m, d);
	// date + interval '3' month (day clamped to the month length like PostgreSQL).
	unsigned mm = m + 3;
	int yy = y;
	while (mm > 12) {
		mm -= 12;
		yy++;
	}
	static constexpr unsigned kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	unsigned dim = kDays[mm - 1];
	if (mm == 2 && ((yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0)) {
		dim = 29;
	}
	p.end_excl = DaysFromCivil(yy, mm, d > dim ? dim : d);
	return p;
}

inline ScanSpec NationSpec() {
	ScanSpec s;
	s.table = "nation";
	s.columns = {{"n_nationkey", ColType::Int32}, {"n_name", ColType::Text}};
	return s;
}

enum : int { kOrdDate = 0, kOrdKey = 1, kOrdCust = 2 };
inline ScanSpec OrdersSpec(const Params &p) {
	ScanSpec s;
	s.table = "orders";
	s.columns = {{"o_orderdate", ColType::Date}, {"o_orderkey", ColType::Int64}, {"o_custkey", ColType::Int64}};
	s.filters = {{kOrdDate, FilterOp::Between, p.start, p.end_excl - 1}};
	return s;
}

enum : int { kLiFlag = 0, kLiOrder = 1, kLiPrice = 2, kLiDisc = 3 };
inline ScanSpec LineitemSpec() {
	ScanSpec s;
	s.table = "lineitem";
	s.columns = {{"l_returnflag", ColType::Text},
	             {"l_orderkey", ColType::Int64},
	             {"l_extendedprice", ColType::Decimal, 2},
	             {"l_discount", ColType::Decimal, 2}};
	FilterSpec f;
	f.column = kLiFlag;
	f.op = FilterOp::TextEq;
	f.strings = {"R"};
	s.filters = {f};
	return s;
}

enum : int { kCKey = 0, kCName = 1, kCBal = 2, kCNation = 3, kCAddr = 4, kCPhone = 5, kCComment = 6 };
inline ScanSpec CustomerSpec() {
	ScanSpec s;
	s.table = "customer";
	s.columns = {{"c_custkey", ColType::Int64}, {"c_name", ColType::Text},    {"c_acctbal", ColType::Decimal, 2},
	             {"c_nationkey", ColType::Int32}, {"c_address", ColType::Text}, {"c_phone", ColType::Text},
	             {"c_comment", ColType::Text}};
	return s;
}

//! Open-addressing int64 -> int64 hash (key INT64_MIN = empty).
struct Slot {
	int64_t key;
	int64_t val;
};
inline constexpr int64_t kEmpty = INT64_MIN;
inline constexpr uint64_t kHashMul = 0x9E3779B97F4A7C15ULL;

struct Hash {
	std::vector<Slot> slots;
	uint64_t mask = 0;
	int shift = 64;
	void Init(std::size_t n) {
		int bits = 4;
		while ((std::size_t(1) << bits) < n * 5 / 2) {
			bits++;
		}
		slots.assign(std::size_t(1) << bits, Slot {kEmpty, 0});
		mask = (uint64_t(1) << bits) - 1;
		shift = 64 - bits;
	}
	std::size_t Home(int64_t k) const {
		return (static_cast<uint64_t>(k) * kHashMul) >> shift;
	}
	//! Insert-or-get: returns the slot.
	Slot &Upsert(int64_t k, bool &inserted) {
		std::size_t s = Home(k);
		while (slots[s].key != kEmpty && slots[s].key != k) {
			s = (s + 1) & mask;
		}
		inserted = slots[s].key == kEmpty;
		slots[s].key = k;
		return slots[s];
	}
	int64_t Find(int64_t k) const { // -1 if absent
		std::size_t s = Home(k);
		while (true) {
			const Slot &sl = slots[s];
			if (sl.key == k) {
				return sl.val;
			}
			if (sl.key == kEmpty) {
				return -1;
			}
			s = (s + 1) & mask;
		}
	}
};

//! Orders restricted to the window: (orderkey, compact customer id) pairs.
struct OrderSet {
	std::vector<int64_t> orderkeys;
	std::vector<int64_t> custkeys; // custkey per order row (before compaction) / compact id table after
};

struct Orders {
	std::vector<int64_t> orderkey;
	std::vector<int32_t> cust_id;      // compact id per order
	std::vector<int64_t> custkeys;     // compact id -> custkey
};

//! Collects (orderkey, custkey) rows from an orders batch.
struct OrderCollector {
	std::vector<int64_t> orderkey;
	std::vector<int64_t> custkey;
	void Add(const Batch &b) {
		const int64_t *ok = b.I64(kOrdKey);
		const int64_t *ck = b.I64(kOrdCust);
		orderkey.insert(orderkey.end(), ok, ok + b.count);
		custkey.insert(custkey.end(), ck, ck + b.count);
	}
};

//! Assign compact customer ids (in first-appearance order of the given rows).
inline Orders Compact(const std::vector<int64_t> &orderkey, const std::vector<int64_t> &custkey) {
	Orders o;
	o.orderkey = orderkey;
	o.cust_id.resize(orderkey.size());
	Hash cmap;
	cmap.Init(orderkey.size() + 1);
	for (std::size_t i = 0; i < orderkey.size(); i++) {
		bool ins;
		Slot &s = cmap.Upsert(custkey[i], ins);
		if (ins) {
			s.val = static_cast<int64_t>(o.custkeys.size());
			o.custkeys.push_back(custkey[i]);
		}
		o.cust_id[i] = static_cast<int32_t>(s.val);
	}
	return o;
}

//! Per-process aggregation state: order hash + per-customer revenue (scale 4).
struct Accumulator {
	Hash orders;
	std::vector<int64_t> rev;
	void Init(const std::vector<int64_t> &orderkey, const std::vector<int32_t> &cust_id, std::size_t ncust) {
		orders.Init(orderkey.size() + 1);
		for (std::size_t i = 0; i < orderkey.size(); i++) {
			bool ins;
			orders.Upsert(orderkey[i], ins).val = cust_id[i];
		}
		rev.assign(ncust + 1, 0);
	}

	//! l_returnflag = 'R' already filtered by the scan.
	void Consume(const Batch &b) {
		const std::size_t n = b.count;
		const int64_t *ACCEL_RESTRICT ok = b.I64(kLiOrder);
		const int64_t *ACCEL_RESTRICT price = b.I64(kLiPrice);
		const int64_t *ACCEL_RESTRICT disc = b.I64(kLiDisc);
		alignas(64) uint32_t home[kBatchRows];
		const Slot *ACCEL_RESTRICT sl = orders.slots.data();
		const uint64_t mask = orders.mask;
		std::size_t i = 0;
#if defined(ACCEL_SIMD_AVX512) && defined(__AVX512DQ__)
		const __m512i mul = _mm512_set1_epi64(static_cast<long long>(kHashMul));
		const __m128i sh = _mm_cvtsi32_si128(orders.shift);
		for (; i + 8 <= n; i += 8) {
			__m512i h = _mm512_srl_epi64(_mm512_mullo_epi64(_mm512_loadu_si512(ok + i), mul), sh);
			_mm256_storeu_si256(reinterpret_cast<__m256i *>(home + i), _mm512_cvtepi64_epi32(h));
		}
#endif
		for (; i < n; i++) {
			home[i] = static_cast<uint32_t>((static_cast<uint64_t>(ok[i]) * kHashMul) >> orders.shift);
		}
		for (i = 0; i < n; i++) {
			ACCEL_PREFETCH(sl + home[i]);
		}
		int64_t *ACCEL_RESTRICT r = rev.data();
		for (i = 0; i < n; i++) {
			std::size_t s = home[i];
			const int64_t k = ok[i];
			while (sl[s].key != k && sl[s].key != kEmpty) {
				s = (s + 1) & mask;
			}
			if (sl[s].key == k) {
				r[sl[s].val] += price[i] * (100 - disc[i]);
			}
		}
	}
};

//! Ranked top-20 compact ids: revenue desc, custkey asc.
inline std::vector<int32_t> TopCustomers(const std::vector<int64_t> &rev, const std::vector<int64_t> &custkeys) {
	std::vector<int32_t> ids;
	for (std::size_t i = 0; i < custkeys.size(); i++) {
		if (rev[i] != 0) {
			ids.push_back(static_cast<int32_t>(i));
		}
	}
	auto less = [&](int32_t a, int32_t b) {
		if (rev[a] != rev[b]) {
			return rev[a] > rev[b];
		}
		return custkeys[a] < custkeys[b];
	};
	const std::size_t k = std::min(kLimit, ids.size());
	std::partial_sort(ids.begin(), ids.begin() + k, ids.end(), less);
	ids.resize(k);
	return ids;
}

struct CustRow {
	int64_t custkey = 0;
	int64_t acctbal = 0;
	int32_t nationkey = 0;
	std::string name, address, phone, comment;
};

//! Finds candidate customers in customer batches (strings copied only for hits).
struct CustomerPicker {
	std::vector<int64_t> cands; // sorted
	std::vector<CustRow> found;
	void Consume(const Batch &b) {
		const int64_t *ck = b.I64(kCKey);
		for (std::size_t i = 0; i < b.count; i++) {
			if (std::binary_search(cands.begin(), cands.end(), ck[i])) {
				CustRow r;
				r.custkey = ck[i];
				r.acctbal = b.I64(kCBal)[i];
				r.nationkey = b.I32(kCNation)[i];
				r.name = std::string(b.Str(kCName)[i].View());
				r.address = std::string(b.Str(kCAddr)[i].View());
				r.phone = std::string(b.Str(kCPhone)[i].View());
				r.comment = std::string(b.Str(kCComment)[i].View());
				found.push_back(std::move(r));
			}
		}
	}
};

inline void PutCust(ByteWriter &w, const CustRow &r) {
	w.Put(r.custkey);
	w.Put(r.acctbal);
	w.Put(r.nationkey);
	w.PutString(r.name);
	w.PutString(r.address);
	w.PutString(r.phone);
	w.PutString(r.comment);
}
inline CustRow GetCust(ByteReader &r) {
	CustRow c;
	c.custkey = r.Get<int64_t>();
	c.acctbal = r.Get<int64_t>();
	c.nationkey = r.Get<int32_t>();
	c.name = r.GetString();
	c.address = r.GetString();
	c.phone = r.GetString();
	c.comment = r.GetString();
	return c;
}

struct NationNames {
	std::vector<std::string> names; // by nationkey
	void Consume(const Batch &b) {
		for (std::size_t i = 0; i < b.count; i++) {
			const int32_t k = b.I32(0)[i];
			if (k >= 0) {
				if (names.size() <= std::size_t(k)) {
					names.resize(k + 1);
				}
				names[k] = std::string(b.Str(1)[i].View());
			}
		}
	}
};

//! Emit the top rows (ranked order) given found customer rows.
inline void Emit(const std::vector<int32_t> &ranked, const std::vector<int64_t> &rev,
                 const std::vector<int64_t> &custkeys, const std::vector<CustRow> &found, const NationNames &nn,
                 ResultWriter &out) {
	for (int32_t id : ranked) {
		const int64_t ck = custkeys[id];
		const CustRow *row = nullptr;
		for (const auto &f : found) {
			if (f.custkey == ck) {
				row = &f;
				break;
			}
		}
		if (!row) {
			continue; // unreachable with FK integrity
		}
		out.Int(ck);
		out.Text(row->name);
		out.Decimal(rev[id], 4);
		out.Decimal(row->acctbal, 2);
		out.Text(std::size_t(row->nationkey) < nn.names.size() ? nn.names[row->nationkey] : std::string());
		out.Text(row->address);
		out.Text(row->phone);
		out.Text(row->comment);
		out.EndRow();
	}
}

} // namespace accelerator_codegen::pg_base::q10
