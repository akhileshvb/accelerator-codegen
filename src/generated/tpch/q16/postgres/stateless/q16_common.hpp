// TPC-H Q16 (stateless PostgreSQL): parts/supplier relationship.
#pragma once

#include "tpch/q2/postgres/stateless/q2_common.hpp"

#include <unordered_map>

namespace accelerator_codegen::pg_stateless::q16 {

using namespace pe_util;

inline constexpr const char *kSqlArgs = "text, text, integer, integer, integer, integer, integer, integer, integer, integer";
inline constexpr const char *kSqlReturns = "TABLE (p_brand text, p_type text, p_size integer, supplier_cnt bigint)";

struct Params {
	std::string brand;
	std::string type_pat; // NOT LIKE
	int32_t sizes[8];
	PatKind kind;
	bool InSizes(int32_t s) const {
		bool r = false;
		for (int i = 0; i < 8; i++) {
			r |= sizes[i] == s;
		}
		return r;
	}
};

inline Params Bind(const Args &args) {
	Params p;
	p.brand = args.Text(0);
	p.type_pat = args.Text(1);
	for (int i = 0; i < 8; i++) {
		p.sizes[i] = static_cast<int32_t>(args.Int(2 + i));
	}
	p.kind = ClassifyPattern(p.type_pat);
	return p;
}

inline void PutParams(ByteWriter &w, const Params &p) {
	w.PutString(p.brand);
	w.PutString(p.type_pat);
	for (int i = 0; i < 8; i++) {
		w.Put<int32_t>(p.sizes[i]);
	}
}
inline Params GetParams(ByteReader &r) {
	Params p;
	p.brand = r.GetString();
	p.type_pat = r.GetString();
	for (int i = 0; i < 8; i++) {
		p.sizes[i] = r.Get<int32_t>();
	}
	p.kind = ClassifyPattern(p.type_pat);
	return p;
}

enum : int { kPPartkey = 0, kPBrand = 1, kPType = 2, kPSize = 3 };

inline ScanSpec PartSpec(const Params &p) {
	ScanSpec s;
	s.table = "part";
	s.columns = {{"p_partkey", ColType::Int64},
	             {"p_brand", ColType::Text},
	             {"p_type", ColType::Text},
	             {"p_size", ColType::Int32}};
	const int32_t lo = *std::min_element(p.sizes, p.sizes + 8);
	const int32_t hi = *std::max_element(p.sizes, p.sizes + 8);
	s.filters = {{kPSize, FilterOp::Between, lo, hi}, {kPBrand, FilterOp::TextNotEq, 0, 0, {p.brand}}};
	return s;
}

inline ScanSpec SupplierSpec() {
	ScanSpec s;
	s.table = "supplier";
	s.columns = {{"s_suppkey", ColType::Int64}, {"s_comment", ColType::Text}};
	return s;
}

//! s_comment LIKE '%Customer%Complaints%'
inline bool IsComplaint(std::string_view c) {
	const auto a = c.find("Customer");
	return a != std::string_view::npos && c.find("Complaints", a + 8) != std::string_view::npos;
}

inline void SupplierConsume(const Batch &b, std::vector<int64_t> &bad) {
	for (std::size_t i = 0; i < b.count; i++) {
		if (IsComplaint(b.Str(1)[i].View())) {
			bad.push_back(b.I64(0)[i]);
		}
	}
}

struct PartRow {
	int64_t key;
	std::string brand, type;
	int32_t size;
};

inline void PartConsume(const Batch &b, const Params &p, std::vector<PartRow> &out) {
	for (std::size_t i = 0; i < b.count; i++) {
		const int32_t size = b.I32(kPSize)[i];
		if (!p.InSizes(size)) {
			continue;
		}
		const std::string_view type = b.Str(kPType)[i].View();
		const bool match = p.kind.kind == PatKind::Eq       ? type == p.kind.lit
		                   : p.kind.kind == PatKind::Prefix ? (type.size() >= p.kind.lit.size() &&
		                                                       type.compare(0, p.kind.lit.size(), p.kind.lit) == 0)
		                                                    : LikeMatch(type, p.type_pat);
		if (match) {
			continue;
		}
		out.push_back({b.I64(kPPartkey)[i], std::string(b.Str(kPBrand)[i].View()), std::string(type), size});
	}
}

struct Group {
	std::string brand, type;
	int32_t size;
};

//! partkey -> group id (dense for keys in [0, cap), sorted big keys otherwise; -1 = filtered out)
//! plus the group table.
struct PartMap {
	std::vector<int32_t> gid; // size cap + 8 (gathers read past the end)
	std::size_t cap = 0;
	std::vector<int64_t> big_keys; // sorted
	std::vector<int32_t> big_gid;
	std::vector<Group> groups;

	int32_t Lookup(int64_t k) const {
		if (ACCEL_LIKELY(static_cast<uint64_t>(k) < cap)) {
			return gid[static_cast<std::size_t>(k)];
		}
		if (big_keys.empty()) {
			return -1;
		}
		const auto it = std::lower_bound(big_keys.begin(), big_keys.end(), k);
		return it != big_keys.end() && *it == k ? big_gid[static_cast<std::size_t>(it - big_keys.begin())] : -1;
	}
	void Put(ByteWriter &w) const {
		w.Put<uint64_t>(cap);
		w.PutVector(gid);
		w.PutVector(big_keys);
		w.PutVector(big_gid);
	}
	static PartMap Get(ByteReader &r) {
		PartMap m;
		m.cap = r.Get<uint64_t>();
		m.gid = r.GetVector<int32_t>();
		m.big_keys = r.GetVector<int64_t>();
		m.big_gid = r.GetVector<int32_t>();
		return m;
	}
};

//! `parts` must have unique keys.
inline PartMap BuildPartMap(const std::vector<PartRow> &parts) {
	PartMap m;
	int64_t mx = -1;
	for (const auto &p : parts) {
		if (p.key >= 0 && p.key < kMaxDenseKey) {
			mx = std::max(mx, p.key);
		}
	}
	m.cap = static_cast<std::size_t>(mx + 1);
	m.gid.assign(m.cap + 8, -1);
	std::unordered_map<std::string, int32_t> ids;
	std::string key;
	std::vector<std::pair<int64_t, int32_t>> big;
	for (const auto &p : parts) {
		key.assign(p.brand);
		key.push_back('\0');
		key.append(p.type);
		key.push_back('\0');
		key.append(reinterpret_cast<const char *>(&p.size), sizeof(p.size));
		auto it = ids.find(key);
		if (it == ids.end()) {
			it = ids.emplace(key, static_cast<int32_t>(m.groups.size())).first;
			m.groups.push_back({p.brand, p.type, p.size});
		}
		if (p.key >= 0 && p.key < kMaxDenseKey) {
			m.gid[static_cast<std::size_t>(p.key)] = it->second;
		} else {
			big.emplace_back(p.key, it->second);
		}
	}
	std::sort(big.begin(), big.end());
	for (const auto &b : big) {
		m.big_keys.push_back(b.first);
		m.big_gid.push_back(b.second);
	}
	return m;
}

enum : int { kSPartkey = 0, kSSuppkey = 1 };

inline ScanSpec PartsuppSpec() {
	ScanSpec s;
	s.table = "partsupp";
	s.columns = {{"ps_partkey", ColType::Int64}, {"ps_suppkey", ColType::Int64}};
	return s;
}

struct Pair {
	int64_t g, s;
	bool operator<(const Pair &o) const {
		return g != o.g ? g < o.g : s < o.s;
	}
	bool operator==(const Pair &o) const {
		return g == o.g && s == o.s;
	}
};

//! Collects (gid, suppkey) pairs for partsupp rows of qualifying parts and non-complaint suppliers.
struct PsState {
	std::vector<Pair> pairs;
	void Consume(const Batch &b, const PartMap &pm, const Flags &bad) {
		const int64_t *ACCEL_RESTRICT pk = b.I64(kSPartkey);
		const int64_t *ACCEL_RESTRICT sk = b.I64(kSSuppkey);
		const int32_t *gid = pm.gid.data();
		const std::size_t n = b.count;
		std::size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
		const bool vec_ok = pm.big_keys.empty();
		const __m512i vcap = _mm512_set1_epi64(static_cast<long long>(pm.cap));
		for (; vec_ok && i + 8 <= n; i += 8) {
			const __m512i k = _mm512_loadu_si512(pk + i);
			const __mmask8 in = _mm512_cmplt_epu64_mask(k, vcap);
			const __m256i g = _mm512_mask_i64gather_epi32(_mm256_set1_epi32(-1), in, k, gid, 4);
			unsigned hit = _mm256_cmpgt_epi32_mask(g, _mm256_set1_epi32(-1));
			if (hit == 0) {
				continue;
			}
			alignas(32) int32_t gs[8];
			_mm256_store_si256(reinterpret_cast<__m256i *>(gs), g);
			while (hit) {
				const int l = __builtin_ctz(hit);
				hit &= hit - 1;
				Add(gs[l], sk[i + l], bad);
			}
		}
#endif
		for (; i < n; i++) {
			const int32_t g = pm.Lookup(pk[i]);
			if (g >= 0) {
				Add(g, sk[i], bad);
			}
		}
	}
	void Add(int32_t g, int64_t s, const Flags &bad) {
		if (bad.Test(s)) {
			return;
		}
		pairs.push_back({g, s});
	}
	void SortUnique() {
		std::sort(pairs.begin(), pairs.end());
		pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
	}
};

inline Flags BadFlags(const std::vector<int64_t> &bad) {
	return MakeFlags(bad);
}

//! `pairs` must be sorted and unique.
inline void Emit(const PartMap &pm, const std::vector<Pair> &pairs, ResultWriter &out) {
	std::vector<int64_t> cnt(pm.groups.size(), 0);
	for (const Pair &v : pairs) {
		cnt[static_cast<std::size_t>(v.g)]++;
	}
	std::vector<uint32_t> order;
	for (std::size_t g = 0; g < cnt.size(); g++) {
		if (cnt[g] > 0) {
			order.push_back(static_cast<uint32_t>(g));
		}
	}
	std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
		if (cnt[a] != cnt[b]) {
			return cnt[a] > cnt[b];
		}
		const Group &x = pm.groups[a], &y = pm.groups[b];
		if (x.brand != y.brand) {
			return x.brand < y.brand;
		}
		if (x.type != y.type) {
			return x.type < y.type;
		}
		return x.size < y.size;
	});
	for (uint32_t g : order) {
		const Group &x = pm.groups[g];
		out.Text(x.brand);
		out.Text(x.type);
		out.Int(x.size);
		out.Int(cnt[g]);
		out.EndRow();
	}
}

} // namespace accelerator_codegen::pg_stateless::q16
