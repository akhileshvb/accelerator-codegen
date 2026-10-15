// TPC-H Q2 (base PostgreSQL): shared helpers (also used by Q11/Q16/Q20 of the
// same group, namespace pe_util), parameters, scan specs and the partsupp kernel.
#pragma once

#include "backends/postgres/api/base/pg_base.hpp"
#include "prims/simd.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace accelerator_codegen::pg_base::pe_util {

//! Keys above this are not stored densely.
inline constexpr int64_t kMaxDenseKey = int64_t(1) << 27;

//! Key set: dense byte flags for keys in [0, cap) plus a sorted list of out-of-range
//! ("big") keys.  The v array has 8 bytes of padding (vector gathers read 4 bytes).
struct Flags {
	std::vector<uint8_t> v; // cap + 8
	std::size_t cap = 0;
	std::vector<int64_t> big; // sorted, unique: keys < 0 or >= kMaxDenseKey

	void Init(std::size_t cap_p) {
		cap = cap_p;
		v.assign(cap + 8, 0);
	}
	bool Test(int64_t k) const {
		if (ACCEL_LIKELY(static_cast<uint64_t>(k) < cap)) {
			return v[static_cast<std::size_t>(k)] != 0;
		}
		return !big.empty() && std::binary_search(big.begin(), big.end(), k);
	}
	//! Slot of a member key in [0, Slots()): the key itself, or cap + rank among big keys.
	std::size_t Index(int64_t k) const {
		if (static_cast<uint64_t>(k) < cap) {
			return static_cast<std::size_t>(k);
		}
		return cap + static_cast<std::size_t>(std::lower_bound(big.begin(), big.end(), k) - big.begin());
	}
	std::size_t Slots() const {
		return cap + big.size();
	}
	void Put(ByteWriter &w) const {
		w.Put<uint64_t>(cap);
		w.PutVector(v);
		w.PutVector(big);
	}
	static Flags Get(ByteReader &r) {
		Flags f;
		f.cap = r.Get<uint64_t>();
		f.v = r.GetVector<uint8_t>();
		f.big = r.GetVector<int64_t>();
		return f;
	}
};

inline Flags MakeFlags(const std::vector<int64_t> &keys) {
	int64_t mx = -1;
	Flags f;
	for (int64_t k : keys) {
		if (k >= 0 && k < kMaxDenseKey) {
			mx = std::max(mx, k);
		} else {
			f.big.push_back(k);
		}
	}
	std::sort(f.big.begin(), f.big.end());
	f.big.erase(std::unique(f.big.begin(), f.big.end()), f.big.end());
	f.Init(static_cast<std::size_t>(mx + 1));
	for (int64_t k : keys) {
		if (k >= 0 && k < kMaxDenseKey) {
			f.v[static_cast<std::size_t>(k)] = 1;
		}
	}
	return f;
}

//! SQL LIKE with '%', '_' and '\' escape.
inline bool LikeMatch(std::string_view t, std::string_view p) {
	std::size_t ti = 0, pi = 0, star = std::string_view::npos, mark = 0;
	while (ti < t.size()) {
		if (pi < p.size() && p[pi] == '%') {
			star = pi++;
			mark = ti;
			continue;
		}
		if (pi < p.size()) {
			if (p[pi] == '\\' && pi + 1 < p.size()) {
				if (p[pi + 1] == t[ti]) {
					pi += 2;
					ti++;
					continue;
				}
			} else if (p[pi] == '_' || p[pi] == t[ti]) {
				pi++;
				ti++;
				continue;
			}
		}
		if (star != std::string_view::npos) {
			pi = star + 1;
			ti = ++mark;
			continue;
		}
		return false;
	}
	while (pi < p.size() && p[pi] == '%') {
		pi++;
	}
	return pi == p.size();
}

//! How a LIKE pattern can be evaluated by the scan: equality, prefix, or in the kernel.
struct PatKind {
	enum Kind { Eq, Prefix, Generic } kind = Generic;
	std::string lit;
};

inline PatKind ClassifyPattern(const std::string &p) {
	PatKind k;
	if (p.find('_') != std::string::npos || p.find('\\') != std::string::npos) {
		return k;
	}
	const auto pct = p.find('%');
	if (pct == std::string::npos) {
		k.kind = PatKind::Eq;
		k.lit = p;
	} else if (pct == p.size() - 1) {
		k.kind = PatKind::Prefix;
		k.lit = p.substr(0, p.size() - 1);
	}
	return k;
}

//! Adds a positive LIKE filter on `column` to the spec when the scan can evaluate it.
//! Returns true when pushed (rows reaching the kernel already match).
inline bool PushLike(ScanSpec &s, int column, const PatKind &k) {
	if (k.kind == PatKind::Eq) {
		s.filters.push_back({column, FilterOp::TextEq, 0, 0, {k.lit}});
		return true;
	}
	if (k.kind == PatKind::Prefix) {
		s.filters.push_back({column, FilterOp::TextPrefix, 0, 0, {k.lit}});
		return true;
	}
	return false;
}

//! sel <- rows i with flagsA[keysA[i]] && flagsB[keysB[i]] (keys outside capacity count as 0).
//! sel needs n + simd::kSelSlack entries.
inline std::size_t SelectBothFlagged(const Flags &fa, const int64_t *ACCEL_RESTRICT keys_a, const Flags &fb,
                                     const int64_t *ACCEL_RESTRICT keys_b, std::size_t n,
                                     uint32_t *ACCEL_RESTRICT sel) {
	const uint8_t *flags_a = fa.v.data();
	const uint8_t *flags_b = fb.v.data();
	const std::size_t cap_a = fa.cap, cap_b = fb.cap;
	(void)flags_a;
	(void)flags_b;
	(void)cap_a;
	(void)cap_b;
	std::size_t count = 0;
	std::size_t i = 0;
	const bool vec_ok = fa.big.empty() && fb.big.empty();
#if defined(ACCEL_SIMD_AVX512)
	const __m512i vcap_a = _mm512_set1_epi64(static_cast<long long>(cap_a));
	const __m512i vcap_b = _mm512_set1_epi64(static_cast<long long>(cap_b));
	const __m256i iota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
	const __m256i ff = _mm256_set1_epi32(0xFF);
	for (; vec_ok && i + 8 <= n; i += 8) {
		const __m512i ka = _mm512_loadu_si512(keys_a + i);
		const __mmask8 in_a = _mm512_cmplt_epu64_mask(ka, vcap_a);
		const __m256i ga = _mm512_mask_i64gather_epi32(_mm256_setzero_si256(), in_a, ka, flags_a, 1);
		const __mmask8 hit_a = _mm256_mask_test_epi32_mask(in_a, ga, ff);
		if (hit_a == 0) {
			continue;
		}
		const __m512i kb = _mm512_loadu_si512(keys_b + i);
		const __mmask8 in_b = _mm512_mask_cmplt_epu64_mask(hit_a, kb, vcap_b);
		const __m256i gb = _mm512_mask_i64gather_epi32(_mm256_setzero_si256(), in_b, kb, flags_b, 1);
		const __mmask8 hit = _mm256_mask_test_epi32_mask(in_b, gb, ff);
		const __m256i idx = _mm256_add_epi32(iota, _mm256_set1_epi32(static_cast<int>(i)));
		_mm256_mask_compressstoreu_epi32(sel + count, hit, idx);
		count += static_cast<std::size_t>(__builtin_popcount(hit));
	}
#endif
	for (; i < n; i++) {
		const uint64_t ka = static_cast<uint64_t>(keys_a[i]);
		const uint64_t kb = static_cast<uint64_t>(keys_b[i]);
		const bool ok = fa.Test(static_cast<int64_t>(ka)) && fb.Test(static_cast<int64_t>(kb));
		sel[count] = static_cast<uint32_t>(i);
		count += ok;
	}
	return count;
}

//! sel <- rows i with flags[keys[i]] (single flag array).
inline std::size_t SelectFlagged(const Flags &f, const int64_t *ACCEL_RESTRICT keys, std::size_t n,
                                 uint32_t *ACCEL_RESTRICT sel) {
	const uint8_t *flags = f.v.data();
	const std::size_t cap = f.cap;
	(void)flags;
	(void)cap;
	std::size_t count = 0;
	std::size_t i = 0;
	const bool vec_ok = f.big.empty();
#if defined(ACCEL_SIMD_AVX512)
	const __m512i vcap = _mm512_set1_epi64(static_cast<long long>(cap));
	const __m256i iota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
	const __m256i ff = _mm256_set1_epi32(0xFF);
	for (; vec_ok && i + 8 <= n; i += 8) {
		const __m512i k = _mm512_loadu_si512(keys + i);
		const __mmask8 in = _mm512_cmplt_epu64_mask(k, vcap);
		const __m256i g = _mm512_mask_i64gather_epi32(_mm256_setzero_si256(), in, k, flags, 1);
		const __mmask8 hit = _mm256_mask_test_epi32_mask(in, g, ff);
		const __m256i idx = _mm256_add_epi32(iota, _mm256_set1_epi32(static_cast<int>(i)));
		_mm256_mask_compressstoreu_epi32(sel + count, hit, idx);
		count += static_cast<std::size_t>(__builtin_popcount(hit));
	}
#endif
	for (; i < n; i++) {
		const uint64_t k = static_cast<uint64_t>(keys[i]);
		const bool ok = f.Test(static_cast<int64_t>(k));
		sel[count] = static_cast<uint32_t>(i);
		count += ok;
	}
	return count;
}

inline void PutStrings(ByteWriter &w, const std::vector<std::string> &v) {
	w.Put<uint64_t>(v.size());
	for (const auto &s : v) {
		w.PutString(s);
	}
}
inline std::vector<std::string> GetStrings(ByteReader &r) {
	std::vector<std::string> v(r.Get<uint64_t>());
	for (auto &s : v) {
		s = r.GetString();
	}
	return v;
}

} // namespace accelerator_codegen::pg_base::pe_util

namespace accelerator_codegen::pg_base::q2 {

using namespace pe_util;

inline constexpr const char *kSqlArgs = "integer, text, text";
inline constexpr const char *kSqlReturns =
    "TABLE (s_acctbal numeric, s_name text, n_name text, p_partkey bigint, p_mfgr text, s_address text, "
    "s_phone text, s_comment text)";

struct Params {
	int32_t size;
	std::string type_pat;
	std::string region;
	PatKind kind;
};

inline Params Bind(const Args &args) {
	Params p;
	p.size = static_cast<int32_t>(args.Int(0));
	p.type_pat = args.Text(1);
	p.region = args.Text(2);
	p.kind = ClassifyPattern(p.type_pat);
	return p;
}

inline void PutParams(ByteWriter &w, const Params &p) {
	w.Put<int32_t>(p.size);
	w.PutString(p.type_pat);
	w.PutString(p.region);
}
inline Params GetParams(ByteReader &r) {
	Params p;
	p.size = r.Get<int32_t>();
	p.type_pat = r.GetString();
	p.region = r.GetString();
	p.kind = ClassifyPattern(p.type_pat);
	return p;
}

struct SuppInfo {
	int64_t key;
	int64_t acctbal;
	int32_t nation;
	std::string name, address, phone, comment;
};

//! Suppliers of the region (small tables, scanned serially) and their key flags.
struct Small {
	std::vector<std::string> nation_names; // index = SuppInfo::nation
	std::vector<SuppInfo> supp;
	Flags flags;
};

inline Small LoadSmall(const Params &p) {
	Small s;
	std::vector<int32_t> regions;
	{
		ScanSpec sp;
		sp.table = "region";
		sp.columns = {{"r_regionkey", ColType::Int32}, {"r_name", ColType::Text}};
		sp.filters = {{1, FilterOp::TextEq, 0, 0, {p.region}}};
		ScanTable(sp, [&](const Batch &b) {
			for (std::size_t i = 0; i < b.count; i++) {
				regions.push_back(b.I32(0)[i]);
			}
		});
	}
	std::vector<int32_t> nation_keys; // parallel to nation_names
	{
		ScanSpec sp;
		sp.table = "nation";
		sp.columns = {{"n_nationkey", ColType::Int32}, {"n_regionkey", ColType::Int32}, {"n_name", ColType::Text}};
		ScanTable(sp, [&](const Batch &b) {
			for (std::size_t i = 0; i < b.count; i++) {
				if (std::find(regions.begin(), regions.end(), b.I32(1)[i]) != regions.end()) {
					nation_keys.push_back(b.I32(0)[i]);
					s.nation_names.emplace_back(b.Str(2)[i].View());
				}
			}
		});
	}
	std::vector<int64_t> skeys;
	if (!nation_keys.empty()) {
		ScanSpec sp;
		sp.table = "supplier";
		sp.columns = {{"s_suppkey", ColType::Int64}, {"s_nationkey", ColType::Int32},
		              {"s_acctbal", ColType::Decimal, 2}, {"s_name", ColType::Text},
		              {"s_address", ColType::Text},      {"s_phone", ColType::Text},
		              {"s_comment", ColType::Text}};
		ScanTable(sp, [&](const Batch &b) {
			for (std::size_t i = 0; i < b.count; i++) {
				const auto it = std::find(nation_keys.begin(), nation_keys.end(), b.I32(1)[i]);
				if (it == nation_keys.end()) {
					continue;
				}
				SuppInfo si;
				si.key = b.I64(0)[i];
				skeys.push_back(si.key);
				si.nation = static_cast<int32_t>(it - nation_keys.begin());
				si.acctbal = b.I64(2)[i];
				si.name = std::string(b.Str(3)[i].View());
				si.address = std::string(b.Str(4)[i].View());
				si.phone = std::string(b.Str(5)[i].View());
				si.comment = std::string(b.Str(6)[i].View());
				s.supp.push_back(std::move(si));
			}
		});
	}
	s.flags = MakeFlags(skeys);
	return s;
}

enum : int { kPPartkey = 0, kPSize = 1, kPType = 2, kPMfgr = 3 };

//! Returns the scan spec and whether the type pattern was pushed into the scan.
inline ScanSpec PartSpec(const Params &p, bool &pushed) {
	ScanSpec s;
	s.table = "part";
	s.columns = {{"p_partkey", ColType::Int64},
	             {"p_size", ColType::Int32},
	             {"p_type", ColType::Text},
	             {"p_mfgr", ColType::Text}};
	s.filters = {{kPSize, FilterOp::Between, p.size, p.size}};
	pushed = PushLike(s, kPType, p.kind);
	return s;
}

struct PartInfo {
	int64_t key;
	std::string mfgr;
};

inline void PartConsume(const Batch &b, const Params &p, bool pushed, std::vector<PartInfo> &out) {
	for (std::size_t i = 0; i < b.count; i++) {
		if (!pushed && !LikeMatch(b.Str(kPType)[i].View(), p.type_pat)) {
			continue;
		}
		out.push_back({b.I64(kPPartkey)[i], std::string(b.Str(kPMfgr)[i].View())});
	}
}

inline Flags PartFlags(const std::vector<PartInfo> &parts) {
	std::vector<int64_t> keys;
	for (const auto &pi : parts) {
		keys.push_back(pi.key);
	}
	return MakeFlags(keys);
}

enum : int { kSPartkey = 0, kSSuppkey = 1, kSCost = 2 };

inline ScanSpec PartsuppSpec(const std::vector<PartInfo> &parts) {
	ScanSpec s;
	s.table = "partsupp";
	s.columns = {{"ps_partkey", ColType::Int64}, {"ps_suppkey", ColType::Int64}, {"ps_supplycost", ColType::Decimal, 2}};
	// Probe partsupp by the (few) qualifying parts through idx_partsupp_partkey.
	FilterSpec f {kSPartkey, FilterOp::IntIn};
	f.ints.reserve(parts.size());
	for (const auto &pi : parts) {
		f.ints.push_back(pi.key);
	}
	s.filters.push_back(std::move(f));
	return s;
}

struct Cand {
	int64_t pk, sk, cost;
};

//! Per-participant partsupp state: running min cost per part and the candidate rows
//! (every row that was <= the running min when seen).
struct PsState {
	std::vector<int64_t> minc;
	std::vector<Cand> cands;
	std::vector<uint32_t> sel;
	void Init(std::size_t part_slots) {
		minc.assign(part_slots, INT64_MAX);
		sel.assign(kBatchRows + simd::kSelSlack, 0);
	}
	void Consume(const Batch &b, const Flags &pf, const Flags &sf) {
		const int64_t *pk = b.I64(kSPartkey);
		const int64_t *sk = b.I64(kSSuppkey);
		const int64_t *cost = b.I64(kSCost);
		const std::size_t n = SelectBothFlagged(pf, pk, sf, sk, b.count, sel.data());
		for (std::size_t j = 0; j < n; j++) {
			const std::size_t i = sel[j];
			int64_t &m = minc[pf.Index(pk[i])];
			if (cost[i] <= m) {
				m = cost[i];
				cands.push_back({pk[i], sk[i], cost[i]});
			}
		}
	}
	//! Candidates equal to their part's min.
	std::vector<Cand> Final(const Flags &pf) const {
		std::vector<Cand> out;
		for (const auto &c : cands) {
			if (c.cost == minc[pf.Index(c.pk)]) {
				out.push_back(c);
			}
		}
		return out;
	}
};

inline void Emit(const Small &small, const std::vector<PartInfo> &parts_unsorted, std::vector<Cand> cands,
                 ResultWriter &out) {
	std::vector<PartInfo> parts = parts_unsorted;
	std::sort(parts.begin(), parts.end(), [](const PartInfo &a, const PartInfo &b) { return a.key < b.key; });
	std::vector<int32_t> sidx(small.flags.Slots(), -1);
	for (std::size_t i = 0; i < small.supp.size(); i++) {
		sidx[small.flags.Index(small.supp[i].key)] = static_cast<int32_t>(i);
	}
	struct Row {
		const SuppInfo *s;
		const std::string *nation;
		const PartInfo *part;
		int64_t pk;
	};
	std::vector<Row> rows;
	rows.reserve(cands.size());
	for (const auto &c : cands) {
		const SuppInfo &s = small.supp[static_cast<std::size_t>(sidx[small.flags.Index(c.sk)])];
		const auto it = std::lower_bound(parts.begin(), parts.end(), c.pk,
		                                 [](const PartInfo &a, int64_t k) { return a.key < k; });
		rows.push_back({&s, &small.nation_names[static_cast<std::size_t>(s.nation)], &*it, c.pk});
	}
	std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
		if (a.s->acctbal != b.s->acctbal) {
			return a.s->acctbal > b.s->acctbal;
		}
		if (*a.nation != *b.nation) {
			return *a.nation < *b.nation;
		}
		if (a.s->name != b.s->name) {
			return a.s->name < b.s->name;
		}
		if (a.pk != b.pk) {
			return a.pk < b.pk;
		}
		return a.s->key < b.s->key;
	});
	if (rows.size() > 100) {
		rows.resize(100);
	}
	for (const auto &r : rows) {
		out.Decimal(r.s->acctbal, 2);
		out.Text(r.s->name);
		out.Text(*r.nation);
		out.Int(r.pk);
		out.Text(r.part->mfgr);
		out.Text(r.s->address);
		out.Text(r.s->phone);
		out.Text(r.s->comment);
		out.EndRow();
	}
}

} // namespace accelerator_codegen::pg_base::q2
