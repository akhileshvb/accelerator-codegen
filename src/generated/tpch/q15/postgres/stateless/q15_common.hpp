// TPC-H Q15 (stateless PostgreSQL): top supplier by quarterly revenue.
// Revenue per l_suppkey lives in a dense __int128 array sized by the supplier
// key domain (keys outside it go to an exact overflow map: they still count in
// MAX(total_revenue) like in the reference revenue0 view).
#pragma once

#include "accelerator_codegen/runtime/postgres_stateless/pg_stateless.hpp"
#include "accelerator_codegen/runtime/simd.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace accelerator_codegen::pg_stateless::q15 {

inline constexpr const char *kSqlArgs = "date";
inline constexpr const char *kSqlReturns =
    "TABLE (s_suppkey bigint, s_name text, s_address text, s_phone text, total_revenue numeric)";

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
	// date + interval '3 month'
	int ny = y;
	unsigned nm = m + 3;
	if (nm > 12) {
		nm -= 12;
		ny++;
	}
	static constexpr unsigned kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	unsigned dim = kDays[nm - 1];
	if (nm == 2 && ((ny % 4 == 0 && ny % 100 != 0) || ny % 400 == 0)) {
		dim = 29;
	}
	p.end_excl = DaysFromCivil(ny, nm, d > dim ? dim : d);
	return p;
}

enum : int { kSuppkey = 0, kPrice = 1, kDiscount = 2 };

inline ScanSpec LineitemSpec(const Params &p) {
	ScanSpec s;
	s.table = "lineitem";
	s.columns = {{"l_suppkey", ColType::Int64},
	             {"l_extendedprice", ColType::Decimal, 2},
	             {"l_discount", ColType::Decimal, 2},
	             {"l_shipdate", ColType::Date}};
	s.filters = {{3, FilterOp::Between, p.start, static_cast<int64_t>(p.end_excl) - 1}};
	return s;
}

struct Supplier {
	int64_t key;
	std::string name, address, phone;
};

//! Serial scan of the small supplier table; also returns the dense domain size.
inline std::vector<Supplier> ScanSuppliers(uint64_t &dense_size) {
	ScanSpec s;
	s.table = "supplier";
	s.columns = {{"s_suppkey", ColType::Int64},
	             {"s_name", ColType::Text},
	             {"s_address", ColType::Text},
	             {"s_phone", ColType::Text}};
	std::vector<Supplier> rows;
	int64_t maxkey = -1;
	bool neg = false;
	ScanTable(s, [&](const Batch &b) {
		const int64_t *k = b.I64(0);
		for (std::size_t i = 0; i < b.count; i++) {
			rows.push_back({k[i], std::string(b.Str(1)[i].View()), std::string(b.Str(2)[i].View()),
			                std::string(b.Str(3)[i].View())});
			maxkey = std::max(maxkey, k[i]);
			neg |= k[i] < 0;
		}
	});
	dense_size = (!neg && maxkey < (int64_t(1) << 24)) ? static_cast<uint64_t>(maxkey + 1) : 0;
	return rows;
}

struct Ent {
	int64_t key;
	__int128 v;
};

struct Revenue {
	std::vector<__int128> sum;
	std::vector<uint8_t> touched;
	std::map<int64_t, __int128> overflow;
	uint64_t dense_size;

	explicit Revenue(uint64_t n) : sum(n, 0), touched(n, 0), dense_size(n) {
	}

	void Consume(const Batch &b) {
		const int64_t *ACCEL_RESTRICT key = b.I64(kSuppkey);
		const int64_t *ACCEL_RESTRICT price = b.I64(kPrice);
		const int64_t *ACCEL_RESTRICT disc = b.I64(kDiscount);
		const std::size_t n = b.count;
		for (std::size_t i = 0; i < n; i++) {
			const __int128 rev = static_cast<__int128>(price[i]) * (100 - disc[i]);
			const uint64_t k = static_cast<uint64_t>(key[i]);
			if (ACCEL_LIKELY(k < dense_size)) {
				sum[k] += rev;
				touched[k] = 1;
			} else {
				overflow[key[i]] += rev;
			}
		}
	}

	void Add(int64_t key, __int128 v) {
		const uint64_t k = static_cast<uint64_t>(key);
		if (k < dense_size) {
			sum[k] += v;
			touched[k] = 1;
		} else {
			overflow[key] += v;
		}
	}

	std::string Serialize() const {
		std::vector<Ent> e;
		for (uint64_t k = 0; k < dense_size; k++) {
			if (touched[k]) {
				e.push_back({static_cast<int64_t>(k), sum[k]});
			}
		}
		for (const auto &kv : overflow) {
			e.push_back({kv.first, kv.second});
		}
		ByteWriter w;
		w.PutVector(e);
		return w.Take();
	}
};

inline void Emit(const Revenue &rev, std::vector<Supplier> &suppliers, ResultWriter &out) {
	bool have = false;
	__int128 best = 0;
	for (uint64_t k = 0; k < rev.dense_size; k++) {
		if (rev.touched[k] && (!have || rev.sum[k] > best)) {
			best = rev.sum[k];
			have = true;
		}
	}
	for (const auto &kv : rev.overflow) {
		if (!have || kv.second > best) {
			best = kv.second;
			have = true;
		}
	}
	if (!have) {
		return;
	}
	std::sort(suppliers.begin(), suppliers.end(), [](const Supplier &a, const Supplier &b) { return a.key < b.key; });
	for (const Supplier &s : suppliers) {
		const uint64_t k = static_cast<uint64_t>(s.key);
		__int128 v;
		if (k < rev.dense_size) {
			if (!rev.touched[k]) {
				continue;
			}
			v = rev.sum[k];
		} else {
			auto it = rev.overflow.find(s.key);
			if (it == rev.overflow.end()) {
				continue;
			}
			v = it->second;
		}
		if (v == best) {
			out.Int(s.key);
			out.Text(s.name);
			out.Text(s.address);
			out.Text(s.phone);
			out.Decimal(v, 4);
			out.EndRow();
		}
	}
}

} // namespace accelerator_codegen::pg_stateless::q15
