// TPC-H Q21 (base PostgreSQL): shared pieces of the single-threaded and
// parallel kernels.
//
// Per order (only o_orderstatus = 'F' orders matter) the EXISTS / NOT EXISTS
// pair collapses to set properties of the order's lineitem suppliers:
//   S = suppliers of the order's lineitems, L = suppliers of its late lines
//   (receiptdate > commitdate).  A late line of supplier s qualifies iff
//   |S| >= 2 and L == {s}; every such line counts once for s.
// Plan: orders scan -> bit vector over F order keys with a rank directory (a
// compact slot id per F order, so the per-order state is a dense 12-byte array
// instead of an array over the sparse key space); lineitem pass with an AVX-512
// gather membership test, slot ranks + prefetch, then branch-light state
// updates {first supplier, multi flag, late supplier, late count, late-multi};
// states of different participants merge associatively.  Final pass counts
// per supplier of the requested nation and groups by name.
#pragma once

#include "backends/postgres/api/base/pg_base.hpp"
#include "prims/simd.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace accelerator_codegen::pg_base::q21 {

inline constexpr const char *kSqlArgs = "text";
inline constexpr const char *kSqlReturns = "TABLE (s_name text, numwait bigint)";

//! Order keys up to this value live in the bit vector, larger ones in a sorted vector.
inline constexpr int64_t kDenseLimit = int64_t(1) << 28;

inline ScanSpec OrdersSpec() {
	ScanSpec s;
	s.table = "orders";
	s.columns = {{"o_orderkey", ColType::Int64}, {"o_orderstatus", ColType::Text}};
	s.filters = {{1, FilterOp::TextEq, 0, 0, {"F"}}};
	return s;
}

enum : int { kLOrder = 0, kLSupp = 1, kLCommit = 2, kLReceipt = 3 };

inline ScanSpec LineitemSpec() {
	ScanSpec s;
	s.table = "lineitem";
	s.columns = {{"l_orderkey", ColType::Int64},
	             {"l_suppkey", ColType::Int64},
	             {"l_commitdate", ColType::Date},
	             {"l_receiptdate", ColType::Date}};
	return s;
}

inline ScanSpec NationSpec(const std::string &name) {
	ScanSpec s;
	s.table = "nation";
	s.columns = {{"n_nationkey", ColType::Int32}, {"n_name", ColType::Text}};
	s.filters = {{1, FilterOp::TextEq, 0, 0, {name}}};
	return s;
}

inline ScanSpec SupplierSpec() {
	ScanSpec s;
	s.table = "supplier";
	s.columns = {{"s_suppkey", ColType::Int64}, {"s_name", ColType::Text}, {"s_nationkey", ColType::Int32}};
	return s;
}

//! Set of F order keys with a rank directory.
struct OrderSet {
	std::vector<uint64_t> words;
	std::vector<uint32_t> cum; // cum[w] = number of set bits in words[0..w)
	std::vector<int64_t> big;  // sorted unique keys > kDenseLimit (or negative)
	int64_t max_dense = 0;     // largest dense key representable (words.size()*64-1 bound)
	uint32_t dense_slots = 0;

	void Consume(const Batch &b) {
		const int64_t *k = b.I64(0);
		for (std::size_t i = 0; i < b.count; i++) {
			if (k[i] < 0 || k[i] > kDenseLimit) {
				big.push_back(k[i]);
				continue;
			}
			const std::size_t w = static_cast<std::size_t>(k[i]) >> 6;
			if (w >= words.size()) {
				words.resize(std::max(w + 1, words.size() * 2), 0);
			}
			words[w] |= uint64_t(1) << (k[i] & 63);
		}
	}
	void Merge(const OrderSet &o) {
		if (o.words.size() > words.size()) {
			words.resize(o.words.size(), 0);
		}
		for (std::size_t i = 0; i < o.words.size(); i++) {
			words[i] |= o.words[i];
		}
		big.insert(big.end(), o.big.begin(), o.big.end());
	}
	//! Sort the overflow keys and build the rank directory.
	void Build() {
		std::sort(big.begin(), big.end());
		big.erase(std::unique(big.begin(), big.end()), big.end());
		if (words.empty()) {
			words.assign(1, 0);
		}
		cum.assign(words.size() + 1, 0);
		uint32_t acc = 0;
		for (std::size_t i = 0; i < words.size(); i++) {
			cum[i] = acc;
			acc += static_cast<uint32_t>(__builtin_popcountll(words[i]));
		}
		cum[words.size()] = acc;
		dense_slots = acc;
		max_dense = static_cast<int64_t>(words.size() * 64) - 1;
	}
	std::size_t Slots() const {
		return static_cast<std::size_t>(dense_slots) + big.size();
	}
	//! Slot of key k, or -1.
	int64_t SlotOf(int64_t k) const {
		if (static_cast<uint64_t>(k) <= static_cast<uint64_t>(max_dense)) {
			const uint64_t uk = static_cast<uint64_t>(k);
			const uint64_t w = words[uk >> 6];
			const uint64_t bit = uint64_t(1) << (uk & 63);
			if (!(w & bit)) {
				return -1;
			}
			return cum[uk >> 6] + __builtin_popcountll(w & (bit - 1));
		}
		const auto it = std::lower_bound(big.begin(), big.end(), k);
		if (it == big.end() || *it != k) {
			return -1;
		}
		return static_cast<int64_t>(dense_slots) + (it - big.begin());
	}
};

inline constexpr int32_t kNone = INT32_MIN;
inline constexpr uint32_t kMultiBit = 1u << 31;     // >= 2 distinct suppliers in the order
inline constexpr uint32_t kLateMultiBit = 1u << 30; // >= 2 distinct late suppliers
inline constexpr uint32_t kCntMask = (1u << 30) - 1;

struct State {
	int32_t first;
	int32_t late;
	uint32_t cnt_flags; // flags | late-line count of `late`
};
static_assert(sizeof(State) == 12, "State layout is serialized");

inline State InitState() {
	return State {kNone, kNone, 0};
}

inline void MergeState(State &a, const State &b) {
	uint32_t flags = (a.cnt_flags | b.cnt_flags) & (kMultiBit | kLateMultiBit);
	if (a.first != kNone && b.first != kNone && a.first != b.first) {
		flags |= kMultiBit;
	}
	uint32_t cnt = 0;
	int32_t late = a.late;
	if (a.late == kNone) {
		late = b.late;
		cnt = b.cnt_flags & kCntMask;
	} else if (b.late == kNone) {
		cnt = a.cnt_flags & kCntMask;
	} else if (a.late == b.late) {
		cnt = (a.cnt_flags & kCntMask) + (b.cnt_flags & kCntMask);
	} else {
		flags |= kLateMultiBit;
		cnt = a.cnt_flags & kCntMask;
	}
	if (a.first == kNone) {
		a.first = b.first;
	}
	a.late = late;
	a.cnt_flags = flags | (cnt & kCntMask);
}

//! Lineitem pass over one batch.
inline void UpdateStates(const Batch &b, const OrderSet &os, State *ACCEL_RESTRICT states, bool &bad) {
	const int64_t *ACCEL_RESTRICT okey = b.I64(kLOrder);
	const int64_t *ACCEL_RESTRICT supp = b.I64(kLSupp);
	const int32_t *ACCEL_RESTRICT commit = b.I32(kLCommit);
	const int32_t *ACCEL_RESTRICT receipt = b.I32(kLReceipt);
	const std::size_t n = b.count;
	uint32_t sel[kBatchRows + 16];
	uint32_t slot[kBatchRows + 16];
	std::size_t ns = 0;
	std::size_t i = 0;
	const bool has_big = !os.big.empty();
	const uint64_t *ACCEL_RESTRICT words = os.words.data();
#if defined(ACCEL_SIMD_AVX512)
	const __m512i vmax = _mm512_set1_epi64(os.max_dense);
	const __m512i one = _mm512_set1_epi64(1);
	const __m512i m63 = _mm512_set1_epi64(63);
	for (; i + 8 <= n; i += 8) {
		const __m512i k = _mm512_loadu_si512(okey + i);
		const __mmask8 inr = _mm512_cmple_epu64_mask(k, vmax);
		const __m512i w =
		    _mm512_mask_i64gather_epi64(_mm512_setzero_si512(), inr, _mm512_srli_epi64(k, 6), words, 8);
		const __m512i bit = _mm512_and_si512(_mm512_srlv_epi64(w, _mm512_and_si512(k, m63)), one);
		unsigned m = _mm512_mask_test_epi64_mask(inr, bit, one);
		if (has_big) {
			m |= static_cast<unsigned>(static_cast<__mmask8>(~inr));
		}
		while (m) {
			const unsigned j = static_cast<unsigned>(__builtin_ctz(m));
			m &= m - 1;
			const int64_t s = os.SlotOf(okey[i + j]);
			if (s >= 0) {
				sel[ns] = static_cast<uint32_t>(i + j);
				slot[ns] = static_cast<uint32_t>(s);
				ACCEL_PREFETCH_W(states + s);
				ns++;
			}
		}
	}
#endif
	for (; i < n; i++) {
		const int64_t s = os.SlotOf(okey[i]);
		if (s >= 0) {
			sel[ns] = static_cast<uint32_t>(i);
			slot[ns] = static_cast<uint32_t>(s);
			ACCEL_PREFETCH_W(states + s);
			ns++;
		}
	}
	bool bad_local = false;
	for (std::size_t j = 0; j < ns; j++) {
		const uint32_t r = sel[j];
		const int64_t s64 = supp[r];
		const int32_t s = static_cast<int32_t>(s64);
		bad_local |= (static_cast<int64_t>(s) != s64) | (s == kNone);
		const bool late = receipt[r] > commit[r];
		State &st = states[slot[j]];
		const int32_t f = st.first;
		uint32_t flags = st.cnt_flags;
		flags |= ((f != kNone) & (f != s)) ? kMultiBit : 0;
		st.first = f == kNone ? s : f;
		if (late) {
			const int32_t l = st.late;
			if (l == kNone) {
				st.late = s;
				flags = (flags & ~kCntMask) | 1u;
			} else if (l == s) {
				flags = (flags & ~kCntMask) | (((flags & kCntMask) + 1) & kCntMask);
			} else {
				flags |= kLateMultiBit;
			}
		}
		st.cnt_flags = flags;
	}
	bad |= bad_local;
}

//! Supplier / nation side (small tables, scanned serially).
struct Suppliers {
	struct Entry {
		int64_t key;
		uint32_t idx;
	};
	std::vector<Entry> by_key; // sorted
	std::vector<std::string> names;

	// Returns false if the nation does not exist.
	bool Load(const std::string &nation) {
		int32_t nationkey = -1;
		bool found = false;
		ScanTable(NationSpec(nation), [&](const Batch &b) {
			if (b.count) {
				nationkey = b.I32(0)[0];
				found = true;
			}
		});
		if (!found) {
			return false;
		}
		ScanTable(SupplierSpec(), [&](const Batch &b) {
			for (std::size_t i = 0; i < b.count; i++) {
				if (b.I32(2)[i] == nationkey) {
					by_key.push_back({b.I64(0)[i], static_cast<uint32_t>(names.size())});
					names.emplace_back(b.Str(1)[i].View());
				}
			}
		});
		std::sort(by_key.begin(), by_key.end(), [](const Entry &a, const Entry &b) { return a.key < b.key; });
		return true;
	}
	int64_t Find(int64_t key) const {
		auto it = std::lower_bound(by_key.begin(), by_key.end(), key,
		                           [](const Entry &e, int64_t k) { return e.key < k; });
		return (it != by_key.end() && it->key == key) ? static_cast<int64_t>(it->idx) : -1;
	}
};

inline void Emit(const OrderSet &os, const std::vector<State> &states, const Suppliers &sup, ResultWriter &out) {
	std::vector<int64_t> per(sup.names.size(), 0);
	for (std::size_t i = 0; i < os.Slots(); i++) {
		const State &st = states[i];
		if ((st.cnt_flags & kMultiBit) && !(st.cnt_flags & kLateMultiBit) && (st.cnt_flags & kCntMask)) {
			const int64_t idx = sup.Find(st.late);
			if (idx >= 0) {
				per[idx] += st.cnt_flags & kCntMask;
			}
		}
	}
	std::map<std::string, int64_t> by_name;
	for (std::size_t i = 0; i < per.size(); i++) {
		if (per[i]) {
			by_name[sup.names[i]] += per[i];
		}
	}
	std::vector<std::pair<std::string, int64_t>> rows(by_name.begin(), by_name.end());
	std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
		return a.second != b.second ? a.second > b.second : a.first < b.first;
	});
	for (const auto &r : rows) {
		out.Text(r.first);
		out.Int(r.second);
		out.EndRow();
	}
}

} // namespace accelerator_codegen::pg_base::q21
