#pragma once

// Query-local aggregation core for TPC-H Q18 (shared by the single-threaded and parallel kernels).
//
//   SELECT l_orderkey, SUM(l_quantity) FROM lineitem GROUP BY l_orderkey HAVING SUM(l_quantity) > T
//
// Design (everything lives only for the duration of one query call):
//  * per-order totals are kept in a dense uint32 array (24 MB at SF1) instead of int64 + a second
//    qualifying array.  A chunk is only taken through the fast path when every quantity is in
//    [0, 2^20) and every key in [0, capacity), so a run of <= 2048 rows cannot exceed 2^31 and a
//    single add wraps at most once (wraps are recorded exactly in a side map).
//  * lineitem is clustered by orderkey, so equal consecutive keys are first combined (SIMD run-end
//    detection + a prefix sum) and the array sees one add per run instead of one per row.
//  * because contributions are non-negative, an order crosses the threshold exactly once: the
//    crossing add records the key as a candidate, so no 6M-entry scan is needed afterwards.
//  * everything unusual (negative / huge quantity, key outside the array, threshold outside
//    [0, 2^31)) goes to an exact "exceptional" row list that is resolved at the end.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/runtime/simd.hpp"

namespace accelerator_codegen {
namespace q18agg {

constexpr int64_t kMaxFastQuantity = int64_t {1} << 20; // exclusive upper bound for the fast path
constexpr int64_t kMaxFastThreshold = int64_t {1} << 31;

//! Zero-initialised uint32 array backed by calloc (lazily mapped zero pages for large sizes).
class TotalsArray {
public:
	TotalsArray() = default;
	TotalsArray(const TotalsArray &) = delete;
	TotalsArray &operator=(const TotalsArray &) = delete;
	~TotalsArray() {
		std::free(data_);
	}
	void Init(size_t n) {
		std::free(data_);
		data_ = static_cast<uint32_t *>(std::calloc(n + 16, sizeof(uint32_t)));
		if (!data_) {
			throw std::bad_alloc();
		}
		cap_ = n;
	}
	//! Single-threaded only.
	void Grow(size_t n) {
		if (n <= cap_) {
			return;
		}
		size_t target = std::max(n, cap_ * 2);
		auto *fresh = static_cast<uint32_t *>(std::calloc(target + 16, sizeof(uint32_t)));
		if (!fresh) {
			throw std::bad_alloc();
		}
		if (data_) {
			std::memcpy(fresh, data_, cap_ * sizeof(uint32_t));
		}
		std::free(data_);
		data_ = fresh;
		cap_ = target;
	}
	uint32_t *data() {
		return data_;
	}
	const uint32_t *data() const {
		return data_;
	}
	size_t capacity() const {
		return cap_;
	}

private:
	uint32_t *data_ = nullptr;
	size_t cap_ = 0;
};

//! Per-worker side state.
struct WorkerAgg {
	std::vector<int64_t> candidates;                         // keys that crossed the threshold
	std::vector<std::pair<int64_t, int64_t>> exceptional;    // rows needing exact slow handling
	std::unordered_map<int64_t, int64_t> carries;            // key -> number of uint32 wraps
};

struct Config {
	uint32_t threshold = 0;  // valid when fast_ok
	bool fast_ok = false;    // threshold in [0, 2^31)
	static Config Make(int64_t threshold_raw) {
		Config c;
		c.fast_ok = threshold_raw >= 0 && threshold_raw < kMaxFastThreshold;
		c.threshold = c.fast_ok ? static_cast<uint32_t>(threshold_raw) : 0;
		return c;
	}
};

template <bool kAtomic>
inline void AddRun(uint32_t *ACCEL_RESTRICT totals, const Config &cfg, WorkerAgg &w, int64_t key, uint32_t sum) {
	uint32_t old;
	if (kAtomic) {
		old = __atomic_fetch_add(&totals[key], sum, __ATOMIC_RELAXED);
	} else {
		old = totals[key];
		totals[key] = old + sum;
	}
	const uint32_t nw = old + sum;
	if (ACCEL_UNLIKELY((nw < old) | ((old <= cfg.threshold) & (nw > cfg.threshold)))) {
		if (nw < old) {
			w.carries[key] += 1;
		} else {
			w.candidates.push_back(key);
		}
	}
}

//! Row-at-a-time insertion (generic / non-flat paths and out-of-contract chunks).
//! kCanGrow: single-threaded only; keys beyond capacity grow the array instead of going exceptional.
template <bool kAtomic, bool kCanGrow>
inline void AddRow(TotalsArray &totals, const Config &cfg, WorkerAgg &w, int64_t key, int64_t q) {
	if (key < 0) {
		return; // orders with negative keys can never be output
	}
	if (kCanGrow && static_cast<uint64_t>(key) >= totals.capacity() && cfg.fast_ok && q >= 0 &&
	    q < kMaxFastQuantity && key < (int64_t {1} << 31)) {
		totals.Grow(static_cast<size_t>(key) + 1);
	}
	if (cfg.fast_ok && q >= 0 && q < kMaxFastQuantity && static_cast<uint64_t>(key) < totals.capacity()) {
		AddRun<kAtomic>(totals.data(), cfg, w, key, static_cast<uint32_t>(q));
		return;
	}
	w.exceptional.emplace_back(key, q);
}

namespace detail {

//! Unsigned max of n int64 values (negative values compare as huge, so max < bound proves 0 <= v < bound).
inline uint64_t MaxU64(const int64_t *ACCEL_RESTRICT v, size_t n) {
	uint64_t mx = 0;
	size_t i = 0;
#if defined(ACCEL_SIMD_AVX512)
	__m512i vmx = _mm512_setzero_si512();
	for (; i + 8 <= n; i += 8) {
		vmx = _mm512_max_epu64(vmx, _mm512_loadu_si512(v + i));
	}
	if (n >= 8) {
		mx = _mm512_reduce_max_epu64(vmx);
	}
#endif
	for (; i < n; i++) {
		mx = std::max(mx, static_cast<uint64_t>(v[i]));
	}
	return mx;
}

//! Bit j of the result is set iff row base+j ends a run of equal keys (keys[j] != keys[j+1], or it is
//! the last row of the chunk).  len <= 64.
inline uint64_t RunEndMask(const int64_t *ACCEL_RESTRICT keys, size_t base, size_t len, size_t n) {
	uint64_t mask = 0;
	size_t j = 0;
#if defined(ACCEL_SIMD_AVX512)
	// compare keys[i..i+8) with keys[i+1..i+9); needs i + 9 <= n
	for (; j + 8 <= len && base + j + 9 <= n; j += 8) {
		const __m512i a = _mm512_loadu_si512(keys + base + j);
		const __m512i b = _mm512_loadu_si512(keys + base + j + 1);
		mask |= static_cast<uint64_t>(_mm512_cmpneq_epi64_mask(a, b)) << j;
	}
#endif
	for (; j < len; j++) {
		const size_t i = base + j;
		const bool end = (i + 1 == n) || keys[i] != keys[i + 1];
		mask |= static_cast<uint64_t>(end) << j;
	}
	return mask;
}

} // namespace detail

//! Aggregate n flat rows.  Rows with a NULL are never passed here.
template <bool kAtomic, bool kCanGrow>
inline void AggregateChunk(TotalsArray &totals, const Config &cfg, WorkerAgg &w, const int64_t *ACCEL_RESTRICT keys,
                           const int64_t *ACCEL_RESTRICT q, size_t n) {
	if (n == 0) {
		return;
	}
	if (n > 2048) { // run sums are only bounded for vector-sized chunks
		for (size_t i = 0; i < n; i++) {
			AddRow<kAtomic, kCanGrow>(totals, cfg, w, keys[i], q[i]);
		}
		return;
	}
	const uint64_t qmax = detail::MaxU64(q, n); // >= 2^63 iff some quantity is negative
	const uint64_t kmax = detail::MaxU64(keys, n);
	const bool q_ok = cfg.fast_ok && qmax < static_cast<uint64_t>(kMaxFastQuantity);
	if (kCanGrow && q_ok && kmax >= totals.capacity() && kmax < (uint64_t {1} << 31)) {
		totals.Grow(static_cast<size_t>(kmax) + 1);
	}
	if (!(q_ok && kmax < totals.capacity())) {
		for (size_t i = 0; i < n; i++) {
			AddRow<kAtomic, kCanGrow>(totals, cfg, w, keys[i], q[i]);
		}
		return;
	}
	uint32_t *ACCEL_RESTRICT arr = totals.data();
	// A run has <= n <= 2048 rows of < 2^20 each, so the run sum fits well below 2^32.
	// Prefix sums are kept in uint32 (wraparound differences stay exact modulo 2^32).
	uint32_t running = 0, prev_end = 0;
	for (size_t base = 0; base < n; base += 64) {
		const size_t len = std::min<size_t>(64, n - base);
		const uint64_t ends = detail::RunEndMask(keys, base, len, n);
		alignas(64) uint32_t prefix[64];
		size_t j = 0;
#if defined(ACCEL_SIMD_AVX512)
		// Inclusive prefix sum of 16 uint32 per step (log-step shifts via valignd with a zero vector).
		const __m512i zero = _mm512_setzero_si512();
		for (; j + 16 <= len; j += 16) {
			const __m256i lo = _mm512_cvtepi64_epi32(_mm512_loadu_si512(q + base + j));
			const __m256i hi = _mm512_cvtepi64_epi32(_mm512_loadu_si512(q + base + j + 8));
			__m512i x = _mm512_inserti64x4(_mm512_castsi256_si512(lo), hi, 1);
			x = _mm512_add_epi32(x, _mm512_alignr_epi32(x, zero, 15));
			x = _mm512_add_epi32(x, _mm512_alignr_epi32(x, zero, 14));
			x = _mm512_add_epi32(x, _mm512_alignr_epi32(x, zero, 12));
			x = _mm512_add_epi32(x, _mm512_alignr_epi32(x, zero, 8));
			x = _mm512_add_epi32(x, _mm512_set1_epi32(static_cast<int>(running)));
			_mm512_storeu_si512(prefix + j, x);
			running = prefix[j + 15];
		}
#endif
		for (; j < len; j++) {
			running += static_cast<uint32_t>(q[base + j]);
			prefix[j] = running;
		}
		uint64_t m = ends;
		while (m) {
			const unsigned e = static_cast<unsigned>(__builtin_ctzll(m));
			m &= m - 1;
			AddRun<kAtomic>(arr, cfg, w, keys[base + e], prefix[e] - prev_end);
			prev_end = prefix[e];
		}
	}
}

//! Result of the lineitem phase: the qualifying orderkeys and their exact SUM(l_quantity).
struct Resolved {
	std::vector<int64_t> keys;                       // sorted, unique
	std::unordered_map<int64_t, int64_t> totals;     // raw (scale 2) sums
};

inline Resolved Resolve(const TotalsArray &totals, int64_t threshold_raw, const std::vector<WorkerAgg *> &workers) {
	Resolved out;
	std::unordered_map<int64_t, int64_t> carry;
	std::unordered_map<int64_t, int64_t> exc;
	std::vector<int64_t> cands;
	for (const auto *w : workers) {
		for (const auto &c : w->carries) {
			carry[c.first] += c.second;
		}
		for (const auto &e : w->exceptional) {
			exc[e.first] += e.second;
		}
		cands.insert(cands.end(), w->candidates.begin(), w->candidates.end());
	}
	std::sort(cands.begin(), cands.end());
	cands.erase(std::unique(cands.begin(), cands.end()), cands.end());
	auto base_total = [&](int64_t k) -> int64_t {
		int64_t t = 0;
		if (static_cast<uint64_t>(k) < totals.capacity()) {
			t = static_cast<int64_t>(totals.data()[k]);
		}
		const auto it = carry.find(k);
		if (it != carry.end()) {
			t += it->second * (int64_t {1} << 32);
		}
		return t;
	};
	for (const auto k : cands) {
		if (exc.find(k) != exc.end()) {
			continue; // resolved below with its exceptional rows
		}
		out.keys.push_back(k);
		out.totals[k] = base_total(k);
	}
	for (const auto &e : exc) {
		const int64_t total = base_total(e.first) + e.second;
		if (total > threshold_raw) {
			out.keys.push_back(e.first);
			out.totals[e.first] = total;
		}
	}
	std::sort(out.keys.begin(), out.keys.end());
	return out;
}

//! Bitmap over the qualifying keys (a few hundred of 6M at SF1).
class KeyBitmap {
public:
	void Build(const std::vector<int64_t> &keys) {
		words_.clear();
		nbits_ = 0;
		if (keys.empty()) {
			return;
		}
		int64_t mx = 0;
		for (auto k : keys) {
			mx = std::max(mx, k);
		}
		nbits_ = static_cast<uint64_t>(mx) + 1;
		words_.assign((nbits_ + 63) / 64 + 1, 0);
		for (auto k : keys) {
			if (k >= 0) {
				words_[static_cast<uint64_t>(k) >> 6] |= uint64_t {1} << (k & 63);
			}
		}
	}
	bool Contains(int64_t k) const {
		return static_cast<uint64_t>(k) < nbits_ && ((words_[static_cast<uint64_t>(k) >> 6] >> (k & 63)) & 1);
	}
	bool Empty() const {
		return nbits_ == 0;
	}
	const uint64_t *words() const {
		return words_.data();
	}
	uint64_t nbits() const {
		return nbits_;
	}

	//! Indexes i in [0,n) with keys[i] in the set; writes them to out (capacity n + kSelSlack).
	size_t Select(const int64_t *ACCEL_RESTRICT keys, size_t n, uint32_t *ACCEL_RESTRICT out) const {
		size_t count = 0, i = 0;
#if defined(ACCEL_SIMD_AVX512)
		const __m512i vn = _mm512_set1_epi64(static_cast<long long>(nbits_));
		const __m256i iota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
		const __m512i one = _mm512_set1_epi64(1), c63 = _mm512_set1_epi64(63);
		for (; i + 8 <= n; i += 8) {
			const __m512i k = _mm512_loadu_si512(keys + i);
			const __mmask8 inr = _mm512_cmplt_epu64_mask(k, vn);
			const __m512i w = _mm512_mask_i64gather_epi64(_mm512_setzero_si512(), inr, _mm512_srli_epi64(k, 6),
			                                              words_.data(), 8);
			const __m512i bit = _mm512_and_si512(_mm512_srlv_epi64(w, _mm512_and_si512(k, c63)), one);
			const __mmask8 hit = _mm512_test_epi64_mask(bit, bit) & inr;
			const __m256i idx = _mm256_add_epi32(iota, _mm256_set1_epi32(static_cast<int>(i)));
			_mm256_mask_compressstoreu_epi32(out + count, hit, idx);
			count += static_cast<size_t>(__builtin_popcount(hit));
		}
#endif
		for (; i < n; i++) {
			out[count] = static_cast<uint32_t>(i);
			count += Contains(keys[i]) ? 1 : 0;
		}
		return count;
	}

private:
	std::vector<uint64_t> words_;
	uint64_t nbits_ = 0;
};

} // namespace q18agg
} // namespace accelerator_codegen
