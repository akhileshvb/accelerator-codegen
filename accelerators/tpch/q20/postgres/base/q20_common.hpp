// TPC-H Q20 (base PostgreSQL): potential part promotion.
// ps_availqty > 0.5 * sum(l_quantity)  <=>  200 * ps_availqty > sum(l_quantity scaled by 100).
#pragma once

#include "tpch/q2/postgres/base/q2_common.hpp"

namespace accelerator_codegen::pg_base::q20 {

using namespace pe_util;

inline constexpr const char *kSqlArgs = "date, text, text";
inline constexpr const char *kSqlReturns = "TABLE (s_name text, s_address text)";

struct Params {
	int32_t start, end_excl; // l_shipdate in [start, end_excl)
	std::string name_pat;    // p_name LIKE
	std::string nation;
	PatKind kind;
};

inline Params Bind(const Args &args) {
	Params p;
	p.start = args.Date(0);
	int y;
	unsigned m, d;
	CivilFromDays(p.start, y, m, d);
	if (m == 2 && d == 29) {
		d = 28;
	}
	p.end_excl = DaysFromCivil(y + 1, m, d);
	p.name_pat = args.Text(1);
	p.nation = args.Text(2);
	p.kind = ClassifyPattern(p.name_pat);
	return p;
}

inline void PutParams(ByteWriter &w, const Params &p) {
	w.Put<int32_t>(p.start);
	w.Put<int32_t>(p.end_excl);
	w.PutString(p.name_pat);
	w.PutString(p.nation);
}
inline Params GetParams(ByteReader &r) {
	Params p;
	p.start = r.Get<int32_t>();
	p.end_excl = r.Get<int32_t>();
	p.name_pat = r.GetString();
	p.nation = r.GetString();
	p.kind = ClassifyPattern(p.name_pat);
	return p;
}

struct SuppInfo {
	int64_t key;
	std::string name, address;
};

//! Suppliers of the nation: infos + key flags.
struct Small {
	std::vector<SuppInfo> supp;
	Flags flags;
};

inline Small LoadSmall(const Params &p) {
	Small s;
	std::vector<int32_t> nk;
	{
		ScanSpec sp;
		sp.table = "nation";
		sp.columns = {{"n_nationkey", ColType::Int32}, {"n_name", ColType::Text}};
		sp.filters = {{1, FilterOp::TextEq, 0, 0, {p.nation}}};
		ScanTable(sp, [&](const Batch &b) {
			for (std::size_t i = 0; i < b.count; i++) {
				nk.push_back(b.I32(0)[i]);
			}
		});
	}
	std::vector<int64_t> skeys;
	for (int32_t n : nk) {
		ScanSpec sp;
		sp.table = "supplier";
		sp.columns = {{"s_suppkey", ColType::Int64},
		              {"s_nationkey", ColType::Int32},
		              {"s_name", ColType::Text},
		              {"s_address", ColType::Text}};
		sp.filters = {{1, FilterOp::Between, n, n}};
		ScanTable(sp, [&](const Batch &b) {
			for (std::size_t i = 0; i < b.count; i++) {
				skeys.push_back(b.I64(0)[i]);
				s.supp.push_back({b.I64(0)[i], std::string(b.Str(2)[i].View()), std::string(b.Str(3)[i].View())});
			}
		});
	}
	s.flags = MakeFlags(skeys);
	return s;
}

inline ScanSpec PartSpec(const Params &p, bool &pushed) {
	ScanSpec s;
	s.table = "part";
	s.columns = {{"p_partkey", ColType::Int64}, {"p_name", ColType::Text}};
	pushed = PushLike(s, 1, p.kind);
	return s;
}

inline void PartConsume(const Batch &b, const Params &p, bool pushed, std::vector<int64_t> &out) {
	for (std::size_t i = 0; i < b.count; i++) {
		if (pushed || LikeMatch(b.Str(1)[i].View(), p.name_pat)) {
			out.push_back(b.I64(0)[i]);
		}
	}
}

inline ScanSpec PartsuppSpec() {
	ScanSpec s;
	s.table = "partsupp";
	s.columns = {{"ps_partkey", ColType::Int64}, {"ps_suppkey", ColType::Int64}, {"ps_availqty", ColType::Int32}};
	return s;
}

struct PsRow {
	int64_t pk, sk;
	int64_t avail;
};

//! Keeps partsupp rows of nation suppliers (the part filter is applied by the caller).
inline void PartsuppConsume(const Batch &b, const Flags &sf, std::vector<uint32_t> &sel, std::vector<PsRow> &out) {
	const std::size_t n = SelectFlagged(sf, b.I64(1), b.count, sel.data());
	for (std::size_t j = 0; j < n; j++) {
		const std::size_t i = sel[j];
		out.push_back({b.I64(0)[i], b.I64(1)[i], b.I32(2)[i]});
	}
}

enum : int { kLShipdate = 0, kLPartkey = 1, kLSuppkey = 2, kLQuantity = 3 };

inline ScanSpec LineitemSpec(const Params &p) {
	ScanSpec s;
	s.table = "lineitem";
	s.columns = {{"l_shipdate", ColType::Date},
	             {"l_partkey", ColType::Int64},
	             {"l_suppkey", ColType::Int64},
	             {"l_quantity", ColType::Decimal, 2}};
	s.filters = {{kLShipdate, FilterOp::Between, p.start, p.end_excl - 1}};
	return s;
}

//! Candidate (partkey, suppkey) pairs with an open-addressing index and key flags for the
//! SIMD pre-filter on lineitem.
struct PairTable {
	std::vector<int64_t> pk, sk;
	std::vector<int32_t> slots; // pair index or -1
	std::size_t mask = 0;
	Flags pflags, sflags;

	static std::size_t Hash(int64_t a, int64_t b) {
		return static_cast<std::size_t>(
		    ((static_cast<uint64_t>(a) * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(b) * 0xC2B2AE3D27D4EB4Full)) >>
		    24);
	}
	//! pk/sk must be set.
	void Build() {
		std::size_t cap = 16;
		while (cap < pk.size() * 4) {
			cap *= 2;
		}
		mask = cap - 1;
		slots.assign(cap, -1);
		for (std::size_t i = 0; i < pk.size(); i++) {
			std::size_t h = Hash(pk[i], sk[i]) & mask;
			while (slots[h] >= 0) {
				h = (h + 1) & mask;
			}
			slots[h] = static_cast<int32_t>(i);
		}
		pflags = MakeFlags(pk);
		sflags = MakeFlags(sk);
	}
	int32_t Find(int64_t a, int64_t b) const {
		std::size_t h = Hash(a, b) & mask;
		for (;;) {
			const int32_t s = slots[h];
			if (s < 0 || (pk[static_cast<std::size_t>(s)] == a && sk[static_cast<std::size_t>(s)] == b)) {
				return s;
			}
			h = (h + 1) & mask;
		}
	}
};

//! Per-pair quantity sums (scaled by 100) and "has lineitem rows" flags.
struct LineState {
	std::vector<int64_t> sum;
	std::vector<uint8_t> seen;
	std::vector<uint32_t> sel;
	explicit LineState(std::size_t pairs) : sum(pairs, 0), seen(pairs, 0), sel(kBatchRows + simd::kSelSlack) {
	}
	void Consume(const Batch &b, const PairTable &t) {
		const int64_t *pk = b.I64(kLPartkey);
		const int64_t *sk = b.I64(kLSuppkey);
		const int64_t *q = b.I64(kLQuantity);
		const std::size_t n = SelectBothFlagged(t.pflags, pk, t.sflags, sk, b.count, sel.data());
		for (std::size_t j = 0; j < n; j++) {
			const std::size_t i = sel[j];
			const int32_t s = t.Find(pk[i], sk[i]);
			if (s >= 0) {
				sum[static_cast<std::size_t>(s)] += q[i];
				seen[static_cast<std::size_t>(s)] = 1;
			}
		}
	}
};

inline void Emit(const Small &small, const PairTable &t, const std::vector<PsRow> &rows,
                 const std::vector<int64_t> &sum, const std::vector<uint8_t> &seen, ResultWriter &out) {
	std::vector<uint8_t> hit(small.flags.Slots(), 0);
	// rows[i] corresponds to pair i.
	for (std::size_t i = 0; i < rows.size(); i++) {
		if (seen[i] && rows[i].avail * 200 > sum[i]) {
			hit[small.flags.Index(t.sk[i])] = 1;
		}
	}
	std::vector<const SuppInfo *> res;
	for (const auto &s : small.supp) {
		if (hit[small.flags.Index(s.key)]) {
			res.push_back(&s);
		}
	}
	std::sort(res.begin(), res.end(), [](const SuppInfo *a, const SuppInfo *b) {
		return a->name != b->name ? a->name < b->name : a->key < b->key;
	});
	for (const auto *s : res) {
		out.Text(s->name);
		out.Text(s->address);
		out.EndRow();
	}
}

//! Builds the pair table from the partsupp rows of nation suppliers whose part qualifies.
inline void BuildPairs(const std::vector<PsRow> &ps, const Flags &partf, std::vector<PsRow> &rows, PairTable &t) {
	for (const auto &r : ps) {
		if (partf.Test(r.pk)) {
			rows.push_back(r);
		}
	}
	std::sort(rows.begin(), rows.end(), [](const PsRow &a, const PsRow &b) {
		return a.pk != b.pk ? a.pk < b.pk : a.sk < b.sk;
	});
	for (const auto &r : rows) {
		t.pk.push_back(r.pk);
		t.sk.push_back(r.sk);
	}
	t.Build();
}

inline Flags KeyFlags(const std::vector<int64_t> &keys) {
	return MakeFlags(keys);
}

} // namespace accelerator_codegen::pg_base::q20
