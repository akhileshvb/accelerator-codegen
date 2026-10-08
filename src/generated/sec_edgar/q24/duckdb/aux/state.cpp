#include "sec_edgar/q24/duckdb/aux/state.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <stdexcept>

#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#define Q24_HAVE_AVX2_DISPATCH 1
#endif

namespace accelerator_codegen::aux_state::query {

// ── hashing ────────────────────────────────────────────────────────────────

uint64_t Q24HashBytes(const char *p, std::size_t n) {
  constexpr uint64_t kMul = 0x9FB21C651E98DF25ull;
  uint64_t h = 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(n) * kMul);
  while (n >= 8) {
    uint64_t w;
    std::memcpy(&w, p, 8);
    h = (h ^ w) * kMul;
    h ^= h >> 32;
    p += 8;
    n -= 8;
  }
  if (n > 0) {
    uint64_t w = 0;
    std::memcpy(&w, p, n);
    h = (h ^ w) * kMul;
    h ^= h >> 32;
  }
  h *= 0xD6E8FEB86659FD93ull;
  h ^= h >> 32;
  return h;
}

namespace {

inline uint64_t MixU64(uint64_t x) {
  x ^= x >> 33;
  x *= 0xFF51AFD7ED558CCDull;
  x ^= x >> 33;
  x *= 0xC4CEB9FE1A85EC53ull;
  x ^= x >> 33;
  return x;
}

constexpr uint64_t kTagMask = 0xFFFFFFFF00000000ull;

std::size_t NextPow2(std::size_t v) {
  std::size_t p = 1;
  while (p < v) p <<= 1;
  return p;
}

} // namespace

// ── string dictionary ──────────────────────────────────────────────────────

uint32_t Q24StringDict::Find(const char *data, uint32_t size,
                             uint64_t hash) const {
  const std::size_t n = slots_.size();
  if (n == 0) return kNone;
  const uint64_t tag = hash & kTagMask;
  for (std::size_t i = SlotOf(hash);; i = (i + 1 == n ? 0 : i + 1)) {
    const uint64_t s = slots_[i];
    if (s == 0) return kNone;
    if ((s & kTagMask) == tag) {
      const uint32_t id = static_cast<uint32_t>(s) - 1;
      const uint32_t len = offs_[id + 1] - offs_[id];
      if (len == size &&
          (size == 0 || std::memcmp(arena_.data() + offs_[id], data, size) == 0))
        return id;
    }
  }
}

uint32_t Q24StringDict::FindOrAdd(const char *data, uint32_t size,
                                  uint64_t hash) {
  // Keep load <= 50% while growing; ShrinkToFit tightens it afterwards.
  if ((static_cast<uint64_t>(this->size()) + 1) * 2 > slots_.size())
    Rehash(slots_.empty() ? 16 : slots_.size() * 2);
  const std::size_t n = slots_.size();
  const uint64_t tag = hash & kTagMask;
  std::size_t i = SlotOf(hash);
  for (;; i = (i + 1 == n ? 0 : i + 1)) {
    const uint64_t s = slots_[i];
    if (s == 0) break;
    if ((s & kTagMask) == tag) {
      const uint32_t id = static_cast<uint32_t>(s) - 1;
      const uint32_t len = offs_[id + 1] - offs_[id];
      if (len == size &&
          (size == 0 || std::memcmp(arena_.data() + offs_[id], data, size) == 0))
        return id;
    }
  }
  if (arena_.size() + size >= std::numeric_limits<uint32_t>::max() - 1 ||
      this->size() >= std::numeric_limits<uint32_t>::max() - 2)
    throw std::length_error("Q24 string dictionary overflow");
  const uint32_t id = this->size();
  arena_.insert(arena_.end(), data, data + size);
  offs_.push_back(static_cast<uint32_t>(arena_.size()));
  slots_[i] = tag | (static_cast<uint64_t>(id) + 1);
  return id;
}

void Q24StringDict::Rehash(std::size_t new_slot_count) {
  slots_.assign(new_slot_count, 0);
  for (uint32_t id = 0; id < size(); ++id) {
    const auto sv = Get(id);
    const uint64_t h = Q24HashBytes(sv.data(), sv.size());
    std::size_t i = SlotOf(h);
    while (slots_[i] != 0) i = (i + 1 == new_slot_count ? 0 : i + 1);
    slots_[i] = (h & kTagMask) | (static_cast<uint64_t>(id) + 1);
  }
}

std::size_t Q24StringDict::Bytes() const {
  return arena_.capacity() + offs_.capacity() * sizeof(uint32_t) +
         slots_.capacity() * sizeof(uint64_t);
}

void Q24StringDict::ShrinkToFit() {
  arena_.shrink_to_fit();
  offs_.shrink_to_fit();
  const std::size_t tight = static_cast<std::size_t>(size() / 0.7) + 2;
  if (tight < slots_.size()) {
    Rehash(tight);
    slots_.shrink_to_fit();
  }
}

// ── pair index ─────────────────────────────────────────────────────────────

std::size_t Q24PairIndex::SlotOf(uint64_t key) const {
  return static_cast<std::size_t>(((MixU64(key) & 0xFFFFFFFFull) * slots_.size()) >> 32);
}

uint32_t Q24PairIndex::Find(uint64_t key) const {
  const std::size_t n = slots_.size();
  if (n == 0) return kNone;
  for (std::size_t i = SlotOf(key);; i = (i + 1 == n ? 0 : i + 1)) {
    const uint32_t s = slots_[i];
    if (s == 0) return kNone;
    if (pairs_[s - 1] == key) return s - 1;
  }
}

uint32_t Q24PairIndex::FindOrAdd(uint64_t key) {
  if ((pairs_.size() + 1) * 2 > slots_.size())
    Rehash(slots_.empty() ? 16 : slots_.size() * 2);
  const std::size_t n = slots_.size();
  std::size_t i = SlotOf(key);
  for (;; i = (i + 1 == n ? 0 : i + 1)) {
    const uint32_t s = slots_[i];
    if (s == 0) break;
    if (pairs_[s - 1] == key) return s - 1;
  }
  pairs_.push_back(key);
  slots_[i] = static_cast<uint32_t>(pairs_.size());
  return static_cast<uint32_t>(pairs_.size() - 1);
}

void Q24PairIndex::Rehash(std::size_t new_slot_count) {
  slots_.assign(new_slot_count, 0);
  for (std::size_t idx = 0; idx < pairs_.size(); ++idx) {
    std::size_t i = SlotOf(pairs_[idx]);
    while (slots_[i] != 0) i = (i + 1 == new_slot_count ? 0 : i + 1);
    slots_[i] = static_cast<uint32_t>(idx + 1);
  }
}

void Q24PairIndex::Reorder(const std::vector<uint32_t> &new_id_of_old) {
  std::vector<uint64_t> reordered(pairs_.size());
  for (std::size_t old = 0; old < pairs_.size(); ++old)
    reordered[new_id_of_old[old]] = pairs_[old];
  pairs_ = std::move(reordered);
  Rehash(slots_.size());
}

std::size_t Q24PairIndex::Bytes() const {
  return pairs_.capacity() * sizeof(uint64_t) +
         slots_.capacity() * sizeof(uint32_t);
}

void Q24PairIndex::ShrinkToFit() {
  pairs_.shrink_to_fit();
  const std::size_t tight = static_cast<std::size_t>(pairs_.size() / 0.7) + 2;
  if (tight < slots_.size()) {
    Rehash(tight);
    slots_.shrink_to_fit();
  }
}

// ── raw pre accumulation / base construction ───────────────────────────────

void Q24RawPre::Add(const Q24Str &a, const Q24Str &t, const Q24Str &v) {
  if (a.null || t.null || v.null) return;
  AddHashed(a, t, v, Q24HashBytes(t.data, t.size), Q24HashBytes(v.data, v.size));
}

void Q24RawPre::AddHashed(const Q24Str &a, const Q24Str &t, const Q24Str &v,
                          uint64_t ht, uint64_t hv) {
  // A pre row with a NULL join key can never match a num row.
  if (a.null || t.null || v.null) return;
  uint32_t aid;
  if (last_adsh_id != Q24StringDict::kNone && last_adsh.size() == a.size &&
      (a.size == 0 || std::memcmp(last_adsh.data(), a.data, a.size) == 0)) {
    aid = last_adsh_id;
  } else {
    aid = adsh.FindOrAdd(a.data, a.size);
    last_adsh_id = aid;
    last_adsh.assign(a.data, a.size);
  }
  rows.push_back(aid);
  rows.push_back(tag.FindOrAdd(t.data, t.size, ht));
  if (last_ver_id != Q24StringDict::kNone && last_ver.size() == v.size &&
      (v.size == 0 || std::memcmp(last_ver.data(), v.data, v.size) == 0)) {
    rows.push_back(last_ver_id);
  } else {
    last_ver_id = version.FindOrAdd(v.data, v.size, hv);
    last_ver.assign(v.data, v.size);
    rows.push_back(last_ver_id);
  }
}

void Q24RawPre::AddBatch(const Q24Str *a, const Q24Str *t, const Q24Str *v,
                         std::size_t n) {
  hash_scratch_.resize(2 * n);
  for (std::size_t i = 0; i < n; ++i) {
    if (t[i].null || v[i].null) continue;
    hash_scratch_[2 * i] = Q24HashBytes(t[i].data, t[i].size);
    hash_scratch_[2 * i + 1] = Q24HashBytes(v[i].data, v[i].size);
    tag.Prefetch(hash_scratch_[2 * i]);
    version.Prefetch(hash_scratch_[2 * i + 1]);
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (t[i].null || v[i].null) continue;
    AddHashed(a[i], t[i], v[i], hash_scratch_[2 * i], hash_scratch_[2 * i + 1]);
  }
}

std::shared_ptr<const Q24PreBase>
BuildQ24PreBase(std::vector<Q24RawPre> &parts) {
  auto base = std::make_shared<Q24PreBase>();
  if (parts.empty()) {
    base->off16.assign(1, 0);
    base->off32.assign(1, 0);
    return base;
  }
  Q24RawPre &g = parts[0];
  for (std::size_t p = 1; p < parts.size(); ++p) {
    Q24RawPre &o = parts[p];
    std::vector<uint32_t> amap(o.adsh.size()), tmap(o.tag.size()),
        vmap(o.version.size());
    for (uint32_t i = 0; i < amap.size(); ++i) {
      const auto s = o.adsh.Get(i);
      amap[i] = g.adsh.FindOrAdd(s.data(), static_cast<uint32_t>(s.size()));
    }
    for (uint32_t i = 0; i < tmap.size(); ++i) {
      const auto s = o.tag.Get(i);
      tmap[i] = g.tag.FindOrAdd(s.data(), static_cast<uint32_t>(s.size()));
    }
    for (uint32_t i = 0; i < vmap.size(); ++i) {
      const auto s = o.version.Get(i);
      vmap[i] = g.version.FindOrAdd(s.data(), static_cast<uint32_t>(s.size()));
    }
    g.rows.reserve(g.rows.size() + o.rows.size());
    for (std::size_t r = 0; r + 2 < o.rows.size(); r += 3) {
      g.rows.push_back(amap[o.rows[r]]);
      g.rows.push_back(tmap[o.rows[r + 1]]);
      g.rows.push_back(vmap[o.rows[r + 2]]);
    }
    o = Q24RawPre();
  }

  const std::size_t n = g.rows.size() / 3;
  if (n >= std::numeric_limits<uint32_t>::max())
    throw std::length_error("Q24 pre row count overflow");
  base->adsh = std::move(g.adsh);
  base->tag = std::move(g.tag);
  base->version = std::move(g.version);
  const uint32_t adsh_count = base->adsh.size();

  // pair index per row (insertion-order ids first)
  std::vector<uint32_t> pair_of(n);
  for (std::size_t r = 0; r < n; ++r) {
    const uint64_t key = (static_cast<uint64_t>(g.rows[r * 3 + 1]) << 32) |
                         g.rows[r * 3 + 2];
    pair_of[r] = base->pairs.FindOrAdd(key);
  }
  // Renumber pairs by descending frequency so common pairs get small ids.
  {
    const std::size_t pair_count = base->pairs.size();
    std::vector<uint32_t> freq(pair_count, 0);
    for (std::size_t r = 0; r < n; ++r) freq[pair_of[r]]++;
    std::vector<uint32_t> order(pair_count);
    for (std::size_t i = 0; i < pair_count; ++i) order[i] = static_cast<uint32_t>(i);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      return freq[a] != freq[b] ? freq[a] > freq[b] : a < b;
    });
    std::vector<uint32_t> new_id(pair_count);
    for (std::size_t rank = 0; rank < pair_count; ++rank) new_id[order[rank]] = static_cast<uint32_t>(rank);
    base->pairs.Reorder(new_id);
    for (std::size_t r = 0; r < n; ++r) pair_of[r] = new_id[pair_of[r]];
  }

  // Tier 0 (ids < 256): bitmap per adsh; tiers 1-2: CSR (counting sort by
  // adsh, then sort + unique each slice).
  base->bits256.assign(static_cast<std::size_t>(adsh_count) * 4, 0);
  uint64_t bit_count = 0;
  for (std::size_t r = 0; r < n; ++r) {
    const uint32_t pid = pair_of[r];
    if (pid < 256) {
      uint64_t &word = base->bits256[static_cast<std::size_t>(g.rows[r * 3]) * 4 + (pid >> 6)];
      const uint64_t mask = 1ull << (pid & 63);
      bit_count += (word & mask) == 0;
      word |= mask;
    }
  }
  const auto build_tier = [&](uint32_t lo, uint32_t hi, auto &out,
                              std::vector<uint32_t> &offsets) {
    using T = typename std::decay_t<decltype(out)>::value_type;
    offsets.assign(static_cast<std::size_t>(adsh_count) + 1, 0);
    for (std::size_t r = 0; r < n; ++r)
      if (pair_of[r] >= lo && pair_of[r] < hi) offsets[g.rows[r * 3] + 1]++;
    for (uint32_t a = 0; a < adsh_count; ++a) offsets[a + 1] += offsets[a];
    out.assign(offsets[adsh_count], T());
    std::vector<uint32_t> cursor(offsets.begin(), offsets.end() - 1);
    for (std::size_t r = 0; r < n; ++r)
      if (pair_of[r] >= lo && pair_of[r] < hi)
        out[cursor[g.rows[r * 3]]++] = static_cast<T>(pair_of[r]);
    std::vector<uint32_t> compact(offsets.size(), 0);
    std::size_t w = 0;
    for (uint32_t a = 0; a < adsh_count; ++a) {
      T *b = out.data() + offsets[a];
      T *e = out.data() + offsets[a + 1];
      std::sort(b, e);
      e = std::unique(b, e);
      const std::size_t len = static_cast<std::size_t>(e - b);
      if (out.data() + w != b) std::memmove(out.data() + w, b, len * sizeof(T));
      w += len;
      compact[a + 1] = static_cast<uint32_t>(w);
    }
    out.resize(w);
    out.shrink_to_fit();
    offsets = std::move(compact);
  };
  build_tier(256, 65536, base->ids16, base->off16);
  build_tier(65536, std::numeric_limits<uint32_t>::max(), base->ids32, base->off32);
  g.rows = std::vector<uint32_t>();
  pair_of = std::vector<uint32_t>();
  base->triple_count = bit_count + base->ids16.size() + base->ids32.size();
  base->adsh.ShrinkToFit();
  base->tag.ShrinkToFit();
  base->version.ShrinkToFit();
  base->pairs.ShrinkToFit();
  return base;
}

namespace {

#ifdef Q24_HAVE_AVX2_DISPATCH
// AVX2 is used only behind a runtime CPU check; the scalar path below is the
// portable reference (ACCEL_Q24_NO_SIMD=1 forces it for A/B testing).
std::atomic<bool> &SimdEnabledFlag() {
  static std::atomic<bool> flag{__builtin_cpu_supports("avx2") &&
                                std::getenv("ACCEL_Q24_NO_SIMD") == nullptr};
  return flag;
}
inline bool UseAvx2() {
  return SimdEnabledFlag().load(std::memory_order_relaxed);
}

// Narrow a sorted slice to <= 64 candidates with a branch-light binary search,
// then test the window with vector equality compares.
__attribute__((target("avx2"))) bool Avx2Contains16(const uint16_t *b,
                                                    std::size_t n, uint16_t key) {
  while (n > 64) {
    const std::size_t mid = n / 2;
    if (b[mid] > key) n = mid;
    else { b += mid; n -= mid; }
  }
  const __m256i k = _mm256_set1_epi16(static_cast<short>(key));
  std::size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b + i));
    if (_mm256_movemask_epi8(_mm256_cmpeq_epi16(x, k))) return true;
  }
  if (i == n) return false;
  if (n >= 16) { // overlapping tail load stays inside the slice
    const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b + n - 16));
    return _mm256_movemask_epi8(_mm256_cmpeq_epi16(x, k)) != 0;
  }
  for (; i < n; ++i)
    if (b[i] == key) return true;
  return false;
}

__attribute__((target("avx2"))) bool Avx2Contains32(const uint32_t *b,
                                                    std::size_t n, uint32_t key) {
  while (n > 64) {
    const std::size_t mid = n / 2;
    if (b[mid] > key) n = mid;
    else { b += mid; n -= mid; }
  }
  const __m256i k = _mm256_set1_epi32(static_cast<int>(key));
  std::size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b + i));
    if (_mm256_movemask_epi8(_mm256_cmpeq_epi32(x, k))) return true;
  }
  if (i == n) return false;
  if (n >= 8) {
    const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b + n - 8));
    return _mm256_movemask_epi8(_mm256_cmpeq_epi32(x, k)) != 0;
  }
  for (; i < n; ++i)
    if (b[i] == key) return true;
  return false;
}
#endif

template <typename T>
inline bool ScalarContains(const T *b, const T *e, T v) {
  if (e - b <= 16) {
    for (; b != e; ++b)
      if (*b == v) return true;
    return false;
  }
  return std::binary_search(b, e, v);
}

inline bool SliceContains(const uint16_t *b, const uint16_t *e, uint16_t v) {
#ifdef Q24_HAVE_AVX2_DISPATCH
  if (UseAvx2()) return Avx2Contains16(b, static_cast<std::size_t>(e - b), v);
#endif
  return ScalarContains(b, e, v);
}

inline bool SliceContains(const uint32_t *b, const uint32_t *e, uint32_t v) {
#ifdef Q24_HAVE_AVX2_DISPATCH
  if (UseAvx2()) return Avx2Contains32(b, static_cast<std::size_t>(e - b), v);
#endif
  return ScalarContains(b, e, v);
}

} // namespace

void Q24SetSimdEnabled(bool enabled) {
#ifdef Q24_HAVE_AVX2_DISPATCH
  SimdEnabledFlag().store(enabled && __builtin_cpu_supports("avx2"));
#else
  (void)enabled;
#endif
}

bool Q24SimdActive() {
#ifdef Q24_HAVE_AVX2_DISPATCH
  return UseAvx2();
#else
  return false;
#endif
}

bool Q24PreBase::Contains(const Q24Str &a, const Q24Str &t,
                          const Q24Str &v) const {
  if (a.null || t.null || v.null) return false;
  const uint32_t aid = adsh.Find(a.data, a.size);
  if (aid == Q24StringDict::kNone) return false;
  const uint32_t tid = tag.Find(t.data, t.size);
  if (tid == Q24StringDict::kNone) return false;
  const uint32_t vid = version.Find(v.data, v.size);
  if (vid == Q24StringDict::kNone) return false;
  const uint32_t pid = pairs.Find((static_cast<uint64_t>(tid) << 32) | vid);
  if (pid == Q24PairIndex::kNone) return false;
  if (pid < 256)
    return (bits256[static_cast<std::size_t>(aid) * 4 + (pid >> 6)] >> (pid & 63)) & 1;
  if (pid < 65536)
    return SliceContains(ids16.data() + off16[aid], ids16.data() + off16[aid + 1],
                         static_cast<uint16_t>(pid));
  return SliceContains(ids32.data() + off32[aid], ids32.data() + off32[aid + 1], pid);
}

void Q24PreBase::ContainsBatch(const Q24Str *a, const Q24Str *t,
                               const Q24Str *v, std::size_t n,
                               uint8_t *out) const {
  struct Work {
    uint64_t ha, ht, hv, key;
    uint32_t aid, pid, vid;
    bool ok, valid;
  };
  thread_local std::vector<Work> work;
  work.resize(n);
  // Adsh values repeat in runs (rows are clustered by filing).
  const auto same_prev = [&](std::size_t i) {
    return i > 0 && work[i - 1].valid && a[i - 1].size == a[i].size &&
           std::memcmp(a[i - 1].data, a[i].data, a[i].size) == 0;
  };
  // Versions repeat even more often than adsh (us-gaap/2023 ...).
  const auto same_prev_ver = [&](std::size_t i) {
    return i > 0 && work[i - 1].valid && v[i - 1].size == v[i].size &&
           std::memcmp(v[i - 1].data, v[i].data, v[i].size) == 0;
  };
  // Stage 1: hash and prefetch dictionary slots.
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = 0;
    auto &w = work[i];
    w.ok = w.valid = !(a[i].null || t[i].null || v[i].null);
    if (!w.ok) continue;
    if (!same_prev(i)) {
      w.ha = Q24HashBytes(a[i].data, a[i].size);
      adsh.Prefetch(w.ha);
    }
    w.ht = Q24HashBytes(t[i].data, t[i].size);
    tag.Prefetch(w.ht);
    if (!same_prev_ver(i)) {
      w.hv = Q24HashBytes(v[i].data, v[i].size);
      version.Prefetch(w.hv);
    }
  }
  // Stage 2: dictionary ids -> pair key; prefetch the pair slot.
  for (std::size_t i = 0; i < n; ++i) {
    auto &w = work[i];
    if (!w.ok) continue;
    w.aid = same_prev(i) ? work[i - 1].aid : adsh.Find(a[i].data, a[i].size, w.ha);
    w.vid = same_prev_ver(i) ? work[i - 1].vid
                             : version.Find(v[i].data, v[i].size, w.hv);
    const uint32_t tid = tag.Find(t[i].data, t[i].size, w.ht);
    if (w.aid == Q24StringDict::kNone || tid == Q24StringDict::kNone ||
        w.vid == Q24StringDict::kNone) {
      w.ok = false;
      continue;
    }
    w.key = (static_cast<uint64_t>(tid) << 32) | w.vid;
    pairs.Prefetch(w.key);
  }
  // Stage 3: pair id; prefetch the CSR slice.
  for (std::size_t i = 0; i < n; ++i) {
    auto &w = work[i];
    w.pid = Q24PairIndex::kNone;
    if (!w.ok) continue;
    w.pid = pairs.Find(w.key);
    if (w.pid == Q24PairIndex::kNone) continue;
    if (w.pid < 256) __builtin_prefetch(bits256.data() + static_cast<std::size_t>(w.aid) * 4);
    else if (w.pid < 65536) __builtin_prefetch(ids16.data() + off16[w.aid]);
    else __builtin_prefetch(ids32.data() + off32[w.aid]);
  }
  // Stage 4: slice search.
  for (std::size_t i = 0; i < n; ++i) {
    const auto &w = work[i];
    if (w.pid == Q24PairIndex::kNone) continue;
    if (w.pid < 256)
      out[i] = (bits256[static_cast<std::size_t>(w.aid) * 4 + (w.pid >> 6)] >> (w.pid & 63)) & 1;
    else if (w.pid < 65536)
      out[i] = SliceContains(ids16.data() + off16[w.aid], ids16.data() + off16[w.aid + 1],
                             static_cast<uint16_t>(w.pid));
    else
      out[i] = SliceContains(ids32.data() + off32[w.aid], ids32.data() + off32[w.aid + 1], w.pid);
  }
}

std::size_t Q24PreBase::Bytes() const {
  return adsh.Bytes() + tag.Bytes() + version.Bytes() + pairs.Bytes() +
         bits256.capacity() * 8 + (off16.capacity() + off32.capacity()) * 4 +
         ids16.capacity() * 2 + ids32.capacity() * 4;
}

std::size_t Q24PreOverlay::Bytes() const {
  std::size_t total = 0;
  for (const auto &k : keys) total += k.capacity() + 48;
  return total;
}

bool Q24PreState::Contains(const Q24Str &a, const Q24Str &t, const Q24Str &v,
                           const std::string &key) const {
  if (a.null || t.null || v.null) return false;
  if (base && base->Contains(a, t, v)) return true;
  return overlay && !overlay->keys.empty() && overlay->keys.count(key) != 0;
}

std::size_t Q24PreState::Bytes() const {
  return (base ? base->Bytes() : 0) + (overlay ? overlay->Bytes() : 0);
}

// ── key encoding ───────────────────────────────────────────────────────────

void Q24AppendComponent(std::string &key, const Q24Str &s) {
  if (s.null) {
    key.push_back('\0');
    return;
  }
  key.push_back('\1');
  const uint32_t n = s.size;
  key.append(reinterpret_cast<const char *>(&n), 4);
  key.append(s.data, s.size);
}

std::size_t Q24ComponentSize(const std::string &key, std::size_t offset) {
  if (key[offset] == '\0') return 1;
  uint32_t n;
  std::memcpy(&n, key.data() + offset + 1, 4);
  return 5 + n;
}

Q24Str Q24ReadComponent(const std::string &key, std::size_t &offset) {
  if (key[offset] == '\0') {
    offset += 1;
    return Q24Str::Null();
  }
  uint32_t n;
  std::memcpy(&n, key.data() + offset + 1, 4);
  Q24Str s = Q24Str::Of(key.data() + offset + 5, n);
  offset += 5 + n;
  return s;
}

// ── sharded accumulator map ────────────────────────────────────────────────

Q24AccMap::Q24AccMap(std::size_t shard_count)
    : shards_(NextPow2(std::max<std::size_t>(1, shard_count))) {}

const Q24Acc *Q24AccMap::Find(const std::string &key) const {
  if (shards_.empty()) return nullptr;
  const auto &shard = shards_[ShardOf(key)];
  if (!shard) return nullptr;
  const auto it = shard->find(key);
  return it == shard->end() ? nullptr : &it->second;
}

std::size_t Q24AccMap::Bytes() const {
  std::size_t total = shards_.capacity() * sizeof(shards_[0]);
  for (const auto &shard : shards_) {
    if (!shard) continue;
    total += sizeof(Shard) + shard->bucket_count() * sizeof(void *);
    for (const auto &e : *shard) total += e.first.capacity() + 32 + sizeof(Q24Acc);
  }
  return total;
}

Q24AccMapTxn::Q24AccMapTxn(const Q24AccMap &base)
    : map_(base), owned_(base.shards_.size()) {}

Q24AccMap::Shard &Q24AccMapTxn::Mutable(std::size_t shard) {
  if (!owned_[shard]) {
    owned_[shard] = map_.shards_[shard]
                        ? std::make_shared<Q24AccMap::Shard>(*map_.shards_[shard])
                        : std::make_shared<Q24AccMap::Shard>();
    map_.shards_[shard] = owned_[shard];
  }
  return *owned_[shard];
}

Q24Acc *Q24AccMapTxn::Find(const std::string &key) {
  const auto &shard = map_.shards_[map_.ShardOf(key)];
  if (!shard) return nullptr;
  const auto it = shard->find(key);
  if (it == shard->end()) return nullptr;
  // Hand out a mutable pointer: clone the shard first.
  auto &mutable_shard = Mutable(map_.ShardOf(key));
  dirty_ = true;
  return &mutable_shard.find(key)->second;
}

Q24Acc &Q24AccMapTxn::GetOrCreate(const std::string &key) {
  auto &shard = Mutable(map_.ShardOf(key));
  dirty_ = true;
  const auto result = shard.emplace(key, Q24Acc());
  if (result.second) map_.size_++;
  return result.first->second;
}

void Q24AccMapTxn::Erase(const std::string &key) {
  const std::size_t s = map_.ShardOf(key);
  if (!map_.shards_[s] || map_.shards_[s]->count(key) == 0) return;
  auto &shard = Mutable(s);
  dirty_ = true;
  map_.size_ -= shard.erase(key);
}

Q24AccMap Q24AccMapTxn::Finish() {
  owned_.clear();
  return std::move(map_);
}

// ── group state ────────────────────────────────────────────────────────────

namespace {

struct Candidate {
  int64_t cnt;
  const std::string *key;
};

int CompareStr(const Q24Str &a, const Q24Str &b) {
  if (a.null || b.null) return a.null == b.null ? 0 : (a.null ? -1 : 1);
  const int c = std::memcmp(a.data, b.data, std::min(a.size, b.size));
  if (c != 0) return c;
  return a.size == b.size ? 0 : (a.size < b.size ? -1 : 1);
}

} // namespace

const std::vector<Q24OutputRow> &Q24GroupState::Result() const {
  std::call_once(once_, [this] {
    std::vector<Candidate> cands;
    groups.ForEach([&](const std::string &key, const Q24Acc &acc) {
      if (acc.cnt >= kSecEdgarQ24MinimumCount) cands.push_back({acc.cnt, &key});
    });
    const auto less = [](const Candidate &l, const Candidate &r) {
      if (l.cnt != r.cnt) return l.cnt > r.cnt;
      std::size_t lo = 0, ro = 0;
      const Q24Str lt = Q24ReadComponent(*l.key, lo);
      const Q24Str rt = Q24ReadComponent(*r.key, ro);
      if (const int c = CompareStr(lt, rt)) return c < 0;
      const Q24Str lv = Q24ReadComponent(*l.key, lo);
      const Q24Str rv = Q24ReadComponent(*r.key, ro);
      return CompareStr(lv, rv) < 0;
    };
    if (cands.size() > kSecEdgarQ24OutputLimit) {
      std::partial_sort(cands.begin(), cands.begin() + kSecEdgarQ24OutputLimit,
                        cands.end(), less);
      cands.resize(kSecEdgarQ24OutputLimit);
    } else {
      std::sort(cands.begin(), cands.end(), less);
    }
    result_.reserve(cands.size());
    for (const auto &c : cands) {
      std::size_t off = 0;
      const Q24Str t = Q24ReadComponent(*c.key, off);
      const Q24Str v = Q24ReadComponent(*c.key, off);
      Q24OutputRow row;
      row.tag_null = t.null;
      row.version_null = v.null;
      if (!t.null) row.tag.assign(t.data, t.size);
      if (!v.null) row.version.assign(v.data, v.size);
      row.count = c.cnt;
      row.total = groups.Find(*c.key)->Total();
      result_.push_back(std::move(row));
    }
  });
  return result_;
}

std::size_t Q24GroupState::Bytes() const { return orphans.Bytes() + groups.Bytes(); }

std::shared_ptr<const Q24GroupState> MakeQ24GroupState(
    const std::unordered_map<std::string, Q24Acc> &orphans) {
  auto state = std::make_shared<Q24GroupState>();
  std::size_t shards = 64;
  while (shards < 65536 && shards * 4 < orphans.size()) shards <<= 1;
  Q24AccMapTxn orphan_txn{Q24AccMap(shards)};
  Q24AccMapTxn group_txn{Q24AccMap(shards)};
  for (const auto &entry : orphans) {
    if (entry.second.cnt <= 0) continue;
    orphan_txn.GetOrCreate(entry.first) = entry.second;
    std::size_t off = Q24ComponentSize(entry.first, 0);
    const std::string gkey = entry.first.substr(off);
    auto &g = group_txn.GetOrCreate(gkey);
    g.cnt += entry.second.cnt;
    g.Add(entry.second.sum);
    g.Add(entry.second.comp);
  }
  state->orphans = orphan_txn.Finish();
  state->groups = group_txn.Finish();
  return state;
}

void Q24OrphanAccumulator::Add(const Q24Str &a, const Q24Str &t,
                               const Q24Str &v, double value) {
  scratch_.clear();
  Q24AppendComponent(scratch_, a);
  Q24AppendComponent(scratch_, t);
  Q24AppendComponent(scratch_, v);
  auto &acc = orphans_[scratch_];
  acc.cnt++;
  acc.Add(value);
}

void Q24OrphanAccumulator::MergeFrom(Q24OrphanAccumulator &other) {
  for (auto &entry : other.orphans_) {
    auto &acc = orphans_[entry.first];
    acc.cnt += entry.second.cnt;
    acc.Add(entry.second.sum);
    acc.Add(entry.second.comp);
  }
  other.orphans_.clear();
}

std::shared_ptr<const Q24GroupState> Q24OrphanAccumulator::Finish() {
  return MakeQ24GroupState(orphans_);
}

} // namespace accelerator_codegen::aux_state::query
